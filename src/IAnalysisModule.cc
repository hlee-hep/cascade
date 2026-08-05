#include "IAnalysisModule.hh"

#include "AnalysisManager.hh"
#include "CacheManager.hh"
#include "ExecutionContext.hh"
#include "Logger.hh"
#include "Provenance.hh"
#include "SnapshotHasher.hh"
#include "Version.hh"

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <utility>

struct IAnalysisModule::Impl
{
    ParamManager Parameters;
    std::string SnapshotHash;
    std::string BaseName = "Interface";
    std::string Name;
    std::string CodeVersionHash;
    std::map<std::string, std::unique_ptr<AnalysisManager>> Managers;
    mutable std::mutex ManagerMutex;
    std::atomic<ModuleStatus> Status{ModuleStatus::Pending};
    mutable std::mutex ResultMutex;
    mutable std::recursive_mutex RunMutex;
    RunResult LastResult;
    ExecutionContext Context;
    bool ExternalRunReserved = false;
    std::optional<PluginOrigin> Origin;
    std::string CacheDecision = "not_checked";
    std::string CacheReason;
};

struct IAnalysisModule::CheckDecision
{
    bool ShouldRun = true;
    std::string Message;
};

namespace
{
struct ScopeExit
{
    std::function<void()> Callback;
    ~ScopeExit() { Callback(); }
};
}

IAnalysisModule::IAnalysisModule() : m_Impl(std::make_unique<Impl>()) { m_Impl->Parameters.RegisterCommon(); }

IAnalysisModule::~IAnalysisModule() = default;

RunResult IAnalysisModule::Run() { return RunImpl_(false); }

void IAnalysisModule::PrepareExternalRun() { PrepareExternalRunWithId(""); }

void IAnalysisModule::PrepareExternalRunWithId(const std::string &runId)
{
    std::lock_guard<std::recursive_mutex> runLock(m_Impl->RunMutex);
    if (m_Impl->ExternalRunReserved || m_Impl->Context.IsActive())
        throw std::runtime_error("Module run is already active: " + Name());
    m_Impl->Parameters.Freeze();
    try
    {
        m_Impl->Context.BeginRunWithId(Name(), BaseName(), runId);
        ProvenanceRecorder::BeginModuleRun(m_Impl->Context.RunId(), Name(), BaseName(), RuntimeLanguage(), true);
        ConfigureProvenance();
        m_Impl->ExternalRunReserved = true;
        SetStatus(ModuleStatus::Initializing);
    }
    catch (...)
    {
        ProvenanceRecorder::DiscardModuleRun(m_Impl->Context.RunId());
        m_Impl->Context.RollbackRun();
        m_Impl->Parameters.Thaw();
        throw;
    }
}

RunResult IAnalysisModule::RunPreparedExternal() { return RunImpl_(true); }

RunResult IAnalysisModule::AdoptExternalRunResult(RunResult result)
{
    std::lock_guard<std::recursive_mutex> runLock(m_Impl->RunMutex);
    if (!m_Impl->ExternalRunReserved) throw std::runtime_error("Module has no reserved external run: " + Name());
    ScopeExit parameterThaw{[this]() { m_Impl->Parameters.Thaw(); }};
    m_Impl->SnapshotHash.clear();
    m_Impl->CacheDecision = "not_checked";
    m_Impl->CacheReason = "cache check not reached";
    m_Impl->ExternalRunReserved = false;
    m_Impl->Context.CleanupExternalRun();
    if (result.Failed() && !result.Exception)
        result.Exception = std::make_exception_ptr(
            std::runtime_error(result.Message.empty() ? "Isolated module failed" : result.Message));
    m_Impl->CacheDecision = result.CacheDecision;
    m_Impl->CacheReason = result.CacheReason;
    return Finish_(result.Status, result.Phase, result.Message, result.Exception);
}

void IAnalysisModule::RequestCancellation() { m_Impl->Context.Cancellation().Request(); }

bool IAnalysisModule::IsCancellationRequested() const
{
    return m_Impl->Context.Cancellation().IsCancellationRequested();
}

RunResult IAnalysisModule::RunImpl_(bool externalPrepared)
{
    std::lock_guard<std::recursive_mutex> runLock(m_Impl->RunMutex);
    if (m_Impl->ExternalRunReserved != externalPrepared)
        throw std::runtime_error(externalPrepared ? "Module has no reserved external run: " + Name()
                                                  : "Module is reserved for isolated execution: " + Name());
    if (externalPrepared)
        m_Impl->ExternalRunReserved = false;
    else
        m_Impl->Parameters.Freeze();
    ScopeExit parameterThaw{[this]() { m_Impl->Parameters.Thaw(); }};
    m_Impl->SnapshotHash.clear();
    m_Impl->CacheDecision = "not_checked";
    m_Impl->CacheReason = "cache check not reached";
    if (UsesAnalysisManagers())
    {
        std::lock_guard<std::mutex> managerLock(m_Impl->ManagerMutex);
        m_Impl->Managers.clear();
        m_Impl->Managers["main"] = std::make_unique<AnalysisManager>();
    }
    SetStatus(ModuleStatus::Initializing);
    try
    {
        if (!m_Impl->Context.IsActive())
        {
            m_Impl->Context.BeginRun(Name(), BaseName());
            ProvenanceRecorder::BeginModuleRun(m_Impl->Context.RunId(), Name(), BaseName(), RuntimeLanguage(), false);
            ConfigureProvenance();
        }
        Init();
    }
    catch (const std::exception &error)
    {
        return Fail_(ModulePhase::Init, error.what(), std::current_exception());
    }
    catch (...)
    {
        return Fail_(ModulePhase::Init, "Unknown exception", std::current_exception());
    }
    if (IsCancellationRequested())
        return Finish_(ModuleStatus::Interrupted, ModulePhase::Init, "Interrupted after initialization");

    CheckDecision decision;
    try
    {
        decision = RunCheck_();
    }
    catch (const std::exception &error)
    {
        return Fail_(ModulePhase::Check, error.what(), std::current_exception());
    }
    catch (...)
    {
        return Fail_(ModulePhase::Check, "Unknown exception", std::current_exception());
    }
    if (!decision.ShouldRun) return Finish_(ModuleStatus::Skipped, ModulePhase::Check, decision.Message);

    SetStatus(ModuleStatus::Running);
    try
    {
        Execute();
    }
    catch (const std::exception &error)
    {
        if (IsCancellationRequested())
        {
            LOG_WARN(Name(), "Execution interrupted: " << error.what());
            return Finish_(ModuleStatus::Interrupted, ModulePhase::Execute, error.what(), std::current_exception());
        }
        return Fail_(ModulePhase::Execute, error.what(), std::current_exception());
    }
    catch (...)
    {
        return Fail_(ModulePhase::Execute, "Unknown exception", std::current_exception());
    }
    if (IsCancellationRequested())
        return Finish_(ModuleStatus::Interrupted, ModulePhase::Execute, "Interrupted during execution");

    SetStatus(ModuleStatus::Finalizing);
    try
    {
        Finalize();
    }
    catch (const std::exception &error)
    {
        return Fail_(ModulePhase::Finalize, error.what(), std::current_exception());
    }
    catch (...)
    {
        return Fail_(ModulePhase::Finalize, "Unknown exception", std::current_exception());
    }
    if (IsCancellationRequested())
        return Finish_(ModuleStatus::Interrupted, ModulePhase::Finalize, "Interrupted during finalization");

    try
    {
        const std::string provenancePath = ProvenanceRecorder::SuccessfulModuleManifestPath(
            m_Impl->Context.OutputDirectory(), m_Impl->Context.RunId());
        const auto outputs = m_Impl->Context.Outputs().StagedOutputs();
        RunResult successful{ModuleStatus::Done, ModulePhase::None, "", nullptr};
        successful.CacheDecision = m_Impl->CacheDecision;
        successful.CacheReason = m_Impl->CacheReason;
        auto manifest = ProvenanceRecorder::BuildModuleRun(
            m_Impl->Context.RunId(), GetMetadata(), m_Impl->CodeVersionHash, m_Impl->SnapshotHash,
            m_Impl->Parameters.DumpJSON(), m_Impl->Context.OutputDirectory(), m_Impl->Context.CacheDirectory(),
            successful, outputs, provenancePath);
        const auto stagedManifest = m_Impl->Context.StageOutput(provenancePath);
        ProvenanceRecorder::WriteModuleRun(manifest, stagedManifest);
        m_Impl->Context.Outputs().Commit();
        ProvenanceRecorder::RefreshOutputIdentities(manifest, m_Impl->Context.OutputDirectory());
        ProvenanceRecorder::WriteModuleRun(manifest, provenancePath);
        CacheManager::AddHash(m_Impl->BaseName, m_Impl->SnapshotHash, m_Impl->Context.CacheDirectory().string(),
                              provenancePath);
        try
        {
            m_Impl->Context.CompleteRun();
        }
        catch (...)
        {
            CacheManager::RemoveHash(m_Impl->BaseName, m_Impl->SnapshotHash,
                                     m_Impl->Context.CacheDirectory().string());
            throw;
        }
        ProvenanceRecorder::StoreModuleRun(manifest);
    }
    catch (const std::exception &error)
    {
        return Fail_(ModulePhase::Commit, error.what(), std::current_exception());
    }
    catch (...)
    {
        return Fail_(ModulePhase::Commit, "Unknown exception", std::current_exception());
    }
    return Finish_(ModuleStatus::Done, ModulePhase::None, "");
}

ModuleMetadata IAnalysisModule::GetMetadata() const
{
    ModuleMetadata info;
    info.Name = BaseName();
    info.Version = CascadeVersionString();
    return info;
}

std::string IAnalysisModule::GetParamsToJSON()
{
    std::lock_guard<std::recursive_mutex> lock(m_Impl->RunMutex);
    return m_Impl->Parameters.DumpJSON();
}

void IAnalysisModule::SetName(const std::string &name)
{
    std::lock_guard<std::recursive_mutex> lock(m_Impl->RunMutex);
    m_Impl->Name = name;
}

void IAnalysisModule::SetBaseName(const std::string &name)
{
    std::lock_guard<std::recursive_mutex> lock(m_Impl->RunMutex);
    m_Impl->BaseName = name;
}

void IAnalysisModule::SetCodeHash(const std::string &hash)
{
    std::lock_guard<std::recursive_mutex> lock(m_Impl->RunMutex);
    m_Impl->CodeVersionHash = hash;
}

void IAnalysisModule::SetPluginOrigin(std::optional<PluginOrigin> origin)
{
    std::lock_guard<std::recursive_mutex> lock(m_Impl->RunMutex);
    m_Impl->Origin = std::move(origin);
}

std::optional<PluginOrigin> IAnalysisModule::GetPluginOrigin() const
{
    std::lock_guard<std::recursive_mutex> lock(m_Impl->RunMutex);
    return m_Impl->Origin;
}

std::string IAnalysisModule::Name() const
{
    std::lock_guard<std::recursive_mutex> lock(m_Impl->RunMutex);
    return m_Impl->Name;
}

std::string IAnalysisModule::BaseName() const
{
    std::lock_guard<std::recursive_mutex> lock(m_Impl->RunMutex);
    return m_Impl->BaseName;
}

std::string IAnalysisModule::GetCodeHash() const
{
    std::lock_guard<std::recursive_mutex> lock(m_Impl->RunMutex);
    return m_Impl->CodeVersionHash;
}

std::string IAnalysisModule::GetRuntimeLanguage() const { return RuntimeLanguage(); }

bool IAnalysisModule::RequiresRootSerialization() const
{
    return UsesAnalysisManagers() || RuntimeLanguage() == "python";
}

ExecutionContext &IAnalysisModule::GetExecutionContext() { return m_Impl->Context; }

const ExecutionContext &IAnalysisModule::GetExecutionContext() const { return m_Impl->Context; }

void IAnalysisModule::SetCacheDirectory(const std::string &path)
{
    std::lock_guard<std::recursive_mutex> lock(m_Impl->RunMutex);
    m_Impl->Context.SetCacheDirectory(path);
}

void IAnalysisModule::SetOutputDirectory(const std::string &path)
{
    std::lock_guard<std::recursive_mutex> lock(m_Impl->RunMutex);
    m_Impl->Context.SetOutputDirectory(path);
}

std::string IAnalysisModule::GetCacheDirectory() const { return m_Impl->Context.CacheDirectory().string(); }

std::string IAnalysisModule::GetOutputDirectory() const { return m_Impl->Context.OutputDirectory().string(); }

std::string IAnalysisModule::GetRunId() const { return m_Impl->Context.RunId(); }

std::string IAnalysisModule::GetLastProvenancePath() const
{
    auto manifest = ProvenanceRecorder::FindModuleRun(m_Impl->Context.RunId());
    if (!manifest) manifest = ProvenanceRecorder::FindLastModuleRun(Name());
    return manifest ? manifest->ManifestPath : std::string();
}

std::string IAnalysisModule::GetLastProvenanceJSON(int indent) const
{
    auto manifest = ProvenanceRecorder::FindModuleRun(m_Impl->Context.RunId());
    if (!manifest) manifest = ProvenanceRecorder::FindLastModuleRun(Name());
    return manifest ? manifest->ToJSON(indent) : std::string();
}

void IAnalysisModule::SetParamValue(const std::string &key, const ParamValue &value)
{
    std::lock_guard<std::recursive_mutex> lock(m_Impl->RunMutex);
    m_Impl->Parameters.SetParamVariant(key, value);
}

ParamValue IAnalysisModule::GetParamValue(const std::string &key) const
{
    std::lock_guard<std::recursive_mutex> lock(m_Impl->RunMutex);
    return m_Impl->Parameters.Get<ParamValue>(key);
}

bool IAnalysisModule::HasParam(const std::string &key) const
{
    std::lock_guard<std::recursive_mutex> lock(m_Impl->RunMutex);
    return m_Impl->Parameters.Has(key);
}

void IAnalysisModule::LoadParamsFromYAML(const std::string &path)
{
    std::lock_guard<std::recursive_mutex> lock(m_Impl->RunMutex);
    m_Impl->Parameters.LoadYAMLFile(path);
}

void IAnalysisModule::LoadParamsFromJSON(const std::string &path)
{
    std::lock_guard<std::recursive_mutex> lock(m_Impl->RunMutex);
    m_Impl->Parameters.LoadJSONFile(path);
}

void IAnalysisModule::SetParamsFromJSON(const std::string &document)
{
    std::lock_guard<std::recursive_mutex> lock(m_Impl->RunMutex);
    m_Impl->Parameters.SetParamsFromJSON(document);
}

void IAnalysisModule::SaveParamsToYAML(const std::string &path)
{
    std::lock_guard<std::recursive_mutex> lock(m_Impl->RunMutex);
    m_Impl->Parameters.SaveYAMLFile(path);
}

void IAnalysisModule::SaveParamsToJSON(const std::string &path)
{
    std::lock_guard<std::recursive_mutex> lock(m_Impl->RunMutex);
    m_Impl->Parameters.SaveJSONFile(path);
}

std::string IAnalysisModule::DumpParamsToYAML(int indent)
{
    std::lock_guard<std::recursive_mutex> lock(m_Impl->RunMutex);
    return m_Impl->Parameters.DumpYAML(indent);
}

std::string IAnalysisModule::DumpParamsToJSON(int indent)
{
    std::lock_guard<std::recursive_mutex> lock(m_Impl->RunMutex);
    return m_Impl->Parameters.DumpJSON(indent);
}

std::string IAnalysisModule::GetStatus() const { return ToString(m_Impl->Status.load()); }

ModuleStatus IAnalysisModule::GetStatusEnum() const { return m_Impl->Status.load(); }

RunResult IAnalysisModule::GetLastRunResult() const
{
    std::lock_guard<std::mutex> lock(m_Impl->ResultMutex);
    return m_Impl->LastResult;
}

ParamManager &IAnalysisModule::GetParamManager() { return m_Impl->Parameters; }

const ParamManager &IAnalysisModule::GetParamManager() const { return m_Impl->Parameters; }

std::map<std::string, double> IAnalysisModule::GetProgressSnapshot() const
{
    std::lock_guard<std::mutex> lock(m_Impl->ManagerMutex);
    std::map<std::string, double> result;
    for (const auto &[name, manager] : m_Impl->Managers) result[name] = manager->GetProgress();
    return result;
}

void IAnalysisModule::SetStatus(ModuleStatus status)
{
    m_Impl->Status.store(status);
    LOG_INFO(Name(), "Status : " << ToString(status));
}

void IAnalysisModule::ConfigureProvenance()
{
    ProvenanceRecorder::SetPluginOrigin(m_Impl->Context.RunId(), m_Impl->Origin);
}

std::string IAnalysisModule::AnalysisSnapshotState() const
{
    std::lock_guard<std::mutex> lock(m_Impl->ManagerMutex);
    nlohmann::json state = nlohmann::json::array();
    for (const auto &[name, manager] : m_Impl->Managers)
        state.push_back({{"name", name}, {"state", nlohmann::json::parse(manager->SnapshotState())}});
    return state.dump();
}

ParamManager &IAnalysisModule::Parameters() { return m_Impl->Parameters; }

const ParamManager &IAnalysisModule::Parameters() const { return m_Impl->Parameters; }

void IAnalysisModule::RegisterAnalysisManager(const std::string &name)
{
    std::lock_guard<std::mutex> lock(m_Impl->ManagerMutex);
    if (m_Impl->Managers.count(name)) throw std::runtime_error("Analysis manager already exists: " + name);
    m_Impl->Managers[name] = std::make_unique<AnalysisManager>();
}

AnalysisManager *IAnalysisModule::GetAnalysisManager(const std::string &name) const
{
    std::lock_guard<std::mutex> lock(m_Impl->ManagerMutex);
    const auto iterator = m_Impl->Managers.find(name);
    return iterator == m_Impl->Managers.end() ? nullptr : iterator->second.get();
}

AnalysisManager *IAnalysisModule::Am(const std::string &name) const { return GetAnalysisManager(name); }

std::filesystem::path IAnalysisModule::StageOutput(const std::filesystem::path &path)
{
    return m_Impl->Context.StageOutput(path);
}

std::filesystem::path IAnalysisModule::FinalOutput(const std::filesystem::path &path) const
{
    return m_Impl->Context.FinalOutput(path);
}

void IAnalysisModule::TrackInput(const std::filesystem::path &path)
{
    ProvenanceRecorder::TrackInput(m_Impl->Context.RunId(), path);
}

ExecutionContext &IAnalysisModule::Context() { return m_Impl->Context; }

const ExecutionContext &IAnalysisModule::Context() const { return m_Impl->Context; }

RunResult IAnalysisModule::Finish_(ModuleStatus status, ModulePhase phase, std::string message,
                                   std::exception_ptr exception)
{
    if (status != ModuleStatus::Done && m_Impl->Context.IsActive()) m_Impl->Context.RollbackRun();
    SetStatus(status);
    RunResult result{status, phase, std::move(message), std::move(exception)};
    result.CacheDecision = m_Impl->CacheDecision;
    result.CacheReason = m_Impl->CacheReason;
    {
        std::lock_guard<std::mutex> lock(m_Impl->ResultMutex);
        m_Impl->LastResult = result;
    }
    FinalizeProvenance_(result);
    return result;
}

void IAnalysisModule::FinalizeProvenance_(const RunResult &result) noexcept
{
    try
    {
        if (ProvenanceRecorder::FindModuleRun(m_Impl->Context.RunId())) return;
        const std::string expectedPath =
            result.Status == ModuleStatus::Done
                ? ProvenanceRecorder::SuccessfulModuleManifestPath(m_Impl->Context.OutputDirectory(),
                                                                   m_Impl->Context.RunId())
                : ProvenanceRecorder::TerminalModuleManifestPath(m_Impl->Context.CacheDirectory(),
                                                                 m_Impl->Context.RunId());
        if (std::filesystem::is_regular_file(expectedPath))
        {
            auto existing = ProvenanceRecorder::LoadModuleRun(expectedPath);
            if (existing.Status == result.Status && existing.Phase == result.Phase)
            {
                ProvenanceRecorder::StoreModuleRun(existing);
                return;
            }
        }
        auto manifest = ProvenanceRecorder::BuildModuleRun(
            m_Impl->Context.RunId(), GetMetadata(), m_Impl->CodeVersionHash, m_Impl->SnapshotHash,
            m_Impl->Parameters.DumpJSON(), m_Impl->Context.OutputDirectory(), m_Impl->Context.CacheDirectory(), result,
            {}, expectedPath);
        ProvenanceRecorder::WriteModuleRun(manifest, expectedPath);
        ProvenanceRecorder::StoreModuleRun(manifest);
    }
    catch (const std::exception &error)
    {
        ProvenanceRecorder::DiscardModuleRun(m_Impl->Context.RunId());
        LOG_ERROR(Name(), "Failed to record provenance: " << error.what());
    }
    catch (...)
    {
        ProvenanceRecorder::DiscardModuleRun(m_Impl->Context.RunId());
        LOG_ERROR(Name(), "Failed to record provenance with an unknown exception");
    }
}

RunResult IAnalysisModule::Fail_(ModulePhase phase, const std::string &message, std::exception_ptr exception)
{
    LOG_ERROR(Name(), "Module failed during " << ToString(phase) << ": " << message);
    InvokeFailureHook_(phase, message);
    return Finish_(ModuleStatus::Failed, phase, message, std::move(exception));
}

void IAnalysisModule::InvokeFailureHook_(ModulePhase phase, const std::string &message)
{
    try
    {
        OnFailure(phase, message);
    }
    catch (const std::exception &error)
    {
        LOG_ERROR(Name(), "OnFailure hook failed: " << error.what());
    }
    catch (...)
    {
        LOG_ERROR(Name(), "OnFailure hook failed with an unknown exception");
    }
}

std::string IAnalysisModule::ComputeSnapshotHash_() const
{
    const std::string artifactHash = m_Impl->Origin ? m_Impl->Origin->ArtifactSha256 : std::string();
    return SnapshotHasher::ComputeSerialized(
        m_Impl->Parameters, m_Impl->BaseName, m_Impl->CodeVersionHash, AnalysisSnapshotState(),
        m_Impl->Context.SnapshotState(), artifactHash,
        ProvenanceRecorder::InputSnapshotState(m_Impl->Context.RunId()));
}

IAnalysisModule::CheckDecision IAnalysisModule::RunCheck_()
{
    if (m_Impl->Parameters.Get<bool>("dry_run"))
    {
        m_Impl->CacheDecision = "not_checked";
        m_Impl->CacheReason = "dry_run enabled";
        LOG_INFO(Name(), "DRY run is enabled. variables and setting will be shown.");
        for (auto &[_, manager] : m_Impl->Managers)
        {
            manager->PrintConfigSummary();
            manager->PrintHistogramSummary();
            manager->PrintCutSummary();
        }
        LOG_INFO("ParamManager", m_Impl->Parameters.DumpJSON());
        return {false, "dry_run enabled"};
    }
    m_Impl->CacheDecision = "checking";
    m_Impl->CacheReason = "evaluating snapshot cache";
    m_Impl->SnapshotHash = ComputeSnapshotHash_();
    if (m_Impl->Parameters.Get<bool>("force_run"))
    {
        m_Impl->CacheDecision = "bypassed";
        m_Impl->CacheReason = "force_run enabled";
        LOG_INFO(Name(), "Force run is enabled. Run will be started.");
        return {true, ""};
    }

    auto cached = CacheManager::Lookup(m_Impl->BaseName, m_Impl->SnapshotHash,
                                       m_Impl->Context.CacheDirectory().string());
    if (!cached)
    {
        m_Impl->CacheDecision = "miss";
        m_Impl->CacheReason = "snapshot not found";
    }
    if (cached)
    {
        std::string reason;
        if (!ProvenanceRecorder::ValidateCachedRun(cached->Provenance, m_Impl->SnapshotHash,
                                                   m_Impl->Context.OutputDirectory(), &reason))
        {
            LOG_WARN(Name(), "Discarding stale snapshot cache entry: " << reason);
            m_Impl->CacheDecision = "miss";
            m_Impl->CacheReason = reason;
            CacheManager::RemoveHash(m_Impl->BaseName, m_Impl->SnapshotHash,
                                     m_Impl->Context.CacheDirectory().string());
            cached.reset();
        }
    }
    if (cached)
    {
        m_Impl->CacheDecision = "hit";
        m_Impl->CacheReason = "snapshot and recorded outputs matched";
        ProvenanceRecorder::SetCacheSource(m_Impl->Context.RunId(), cached->Provenance);
        LOG_INFO(Name(), "Matching snapshot is already cached.");
    }
    return {!cached, cached ? "snapshot already cached" : ""};
}
