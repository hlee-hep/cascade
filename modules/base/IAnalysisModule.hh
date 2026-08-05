#pragma once
#include "AnalysisManager.hh"
#include "CacheManager.hh"
#include "ExecutionContext.hh"
#include "InterruptManager.hh"
#include "Logger.hh"
#include "ModuleMetadata.hh"
#include "ModuleRun.hh"
#include "ParamManager.hh"
#include "Provenance.hh"
#include "SnapshotHasher.hh"
#include "sha256.hh"
#include "Version.hh"
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include <yaml-cpp/yaml.h>

class AnalysisManager;

class IAnalysisModule
{
  public:
    IAnalysisModule() { m_Param.RegisterCommon(); }
    virtual ~IAnalysisModule() = default;

    RunResult Run() { return RunImpl_(false); }

    void PrepareExternalRun() { PrepareExternalRunWithId(""); }

    void PrepareExternalRunWithId(const std::string &runId)
    {
        std::lock_guard<std::recursive_mutex> runLock(m_RunMutex);
        if (m_ExternalRunReserved || m_Context.IsActive()) throw std::runtime_error("Module run is already active: " + Name());
        m_Param.Freeze();
        try
        {
            m_Context.BeginRunWithId(Name(), BaseName(), runId);
            ProvenanceRecorder::BeginModuleRun(m_Context.RunId(), Name(), BaseName(), RuntimeLanguage(), true);
            ConfigureProvenance();
            m_ExternalRunReserved = true;
            SetStatus(ModuleStatus::Initializing);
        }
        catch (...)
        {
            ProvenanceRecorder::DiscardModuleRun(m_Context.RunId());
            m_Context.RollbackRun();
            m_Param.Thaw();
            throw;
        }
    }

    RunResult RunPreparedExternal() { return RunImpl_(true); }

    RunResult AdoptExternalRunResult(RunResult result)
    {
        std::lock_guard<std::recursive_mutex> runLock(m_RunMutex);
        if (!m_ExternalRunReserved) throw std::runtime_error("Module has no reserved external run: " + Name());
        struct ParameterThaw
        {
            ParamManager &Parameters;
            ~ParameterThaw() { Parameters.Thaw(); }
        } parameterThaw{m_Param};
        m_Hash.clear();
        m_CacheDecision = "not_checked";
        m_CacheReason = "cache check not reached";
        m_ExternalRunReserved = false;
        m_Context.CleanupExternalRun();
        if (result.Failed() && !result.Exception)
            result.Exception = std::make_exception_ptr(std::runtime_error(result.Message.empty() ? "Isolated module failed" : result.Message));
        m_CacheDecision = result.CacheDecision;
        m_CacheReason = result.CacheReason;
        return Finish_(result.Status, result.Phase, result.Message, result.Exception);
    }

    void RequestCancellation() { m_Context.Cancellation().Request(); }
    bool IsCancellationRequested() const { return m_Context.Cancellation().IsCancellationRequested(); }

  private:
    RunResult RunImpl_(bool externalPrepared)
    {
        std::lock_guard<std::recursive_mutex> runLock(m_RunMutex);
        if (m_ExternalRunReserved != externalPrepared)
            throw std::runtime_error(externalPrepared ? "Module has no reserved external run: " + Name()
                                                      : "Module is reserved for isolated execution: " + Name());
        if (externalPrepared)
            m_ExternalRunReserved = false;
        else
            m_Param.Freeze();
        struct ParameterThaw
        {
            ParamManager &Parameters;
            ~ParameterThaw() { Parameters.Thaw(); }
        } parameterThaw{m_Param};
        if (UsesAnalysisManagers())
        {
            std::lock_guard<std::mutex> managerLock(m_ManagerMutex);
            m_Managers.clear();
            m_Managers["main"] = std::make_unique<AnalysisManager>();
        }
        SetStatus(ModuleStatus::Initializing);
        try
        {
            if (!m_Context.IsActive())
            {
                m_Context.BeginRun(Name(), BaseName());
                ProvenanceRecorder::BeginModuleRun(m_Context.RunId(), Name(), BaseName(), RuntimeLanguage(), false);
                ConfigureProvenance();
            }
            Init();
        }
        catch (const std::exception &e)
        {
            return Fail_(ModulePhase::Init, e.what(), std::current_exception());
        }
        catch (...)
        {
            return Fail_(ModulePhase::Init, "Unknown exception", std::current_exception());
        }
        if (IsCancellationRequested())
        {
            return Finish_(ModuleStatus::Interrupted, ModulePhase::Init, "Interrupted after initialization");
        }
        CheckDecision decision;
        try
        {
            decision = RunCheck_();
        }
        catch (const std::exception &e)
        {
            return Fail_(ModulePhase::Check, e.what(), std::current_exception());
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
        catch (const std::exception &e)
        {
            if (IsCancellationRequested())
            {
                LOG_WARN(Name(), "Execution interrupted: " << e.what());
                return Finish_(ModuleStatus::Interrupted, ModulePhase::Execute, e.what(), std::current_exception());
            }
            return Fail_(ModulePhase::Execute, e.what(), std::current_exception());
        }
        catch (...)
        {
            return Fail_(ModulePhase::Execute, "Unknown exception", std::current_exception());
        }
        if (IsCancellationRequested())
        {
            return Finish_(ModuleStatus::Interrupted, ModulePhase::Execute, "Interrupted during execution");
        }
        SetStatus(ModuleStatus::Finalizing);
        try
        {
            Finalize();
        }
        catch (const std::exception &e)
        {
            return Fail_(ModulePhase::Finalize, e.what(), std::current_exception());
        }
        catch (...)
        {
            return Fail_(ModulePhase::Finalize, "Unknown exception", std::current_exception());
        }

        if (IsCancellationRequested())
        {
            return Finish_(ModuleStatus::Interrupted, ModulePhase::Finalize, "Interrupted during finalization");
        }

        try
        {
            const std::string provenancePath =
                ProvenanceRecorder::SuccessfulModuleManifestPath(m_Context.OutputDirectory(), m_Context.RunId());
            const auto outputs = m_Context.Outputs().StagedOutputs();
            RunResult successful{ModuleStatus::Done, ModulePhase::None, "", nullptr};
            successful.CacheDecision = m_CacheDecision;
            successful.CacheReason = m_CacheReason;
            auto manifest = ProvenanceRecorder::BuildModuleRun(
                m_Context.RunId(), GetMetadata(), m_CodeVersionHash, m_Hash, m_Param.DumpJSON(),
                m_Context.OutputDirectory(), m_Context.CacheDirectory(), successful, outputs, provenancePath);
            const auto stagedManifest = m_Context.StageOutput(provenancePath);
            ProvenanceRecorder::WriteModuleRun(manifest, stagedManifest);
            m_Context.Outputs().Commit();
            ProvenanceRecorder::RefreshOutputIdentities(manifest, m_Context.OutputDirectory());
            ProvenanceRecorder::WriteModuleRun(manifest, provenancePath);
            CacheManager::AddHash(m_Basename, m_Hash, m_Context.CacheDirectory().string(), provenancePath);
            try
            {
                m_Context.CompleteRun();
            }
            catch (...)
            {
                CacheManager::RemoveHash(m_Basename, m_Hash, m_Context.CacheDirectory().string());
                throw;
            }
            ProvenanceRecorder::StoreModuleRun(manifest);
        }
        catch (const std::exception &e)
        {
            return Fail_(ModulePhase::Commit, e.what(), std::current_exception());
        }
        catch (...)
        {
            return Fail_(ModulePhase::Commit, "Unknown exception", std::current_exception());
        }
        return Finish_(ModuleStatus::Done, ModulePhase::None, "");
    }

  public:
    virtual void Description() const = 0;
    virtual ModuleMetadata GetMetadata() const
    {
        ModuleMetadata info;
        info.Name = BaseName();
        info.Version = CascadeVersionString();
        return info;
    }

    std::string GetParamsToJSON()
    {
        std::lock_guard<std::recursive_mutex> lock(m_RunMutex);
        return m_Param.DumpJSON();
    }

    void SetName(const std::string &name)
    {
        std::lock_guard<std::recursive_mutex> lock(m_RunMutex);
        m_Name = name;
    }
    void SetBaseName(const std::string &name)
    {
        std::lock_guard<std::recursive_mutex> lock(m_RunMutex);
        m_Basename = name;
    }
    void SetCodeHash(const std::string &hash)
    {
        std::lock_guard<std::recursive_mutex> lock(m_RunMutex);
        m_CodeVersionHash = hash;
    }
    void SetPluginOrigin(std::optional<PluginOrigin> origin)
    {
        std::lock_guard<std::recursive_mutex> lock(m_RunMutex);
        m_PluginOrigin = std::move(origin);
    }
    std::optional<PluginOrigin> GetPluginOrigin() const
    {
        std::lock_guard<std::recursive_mutex> lock(m_RunMutex);
        return m_PluginOrigin;
    }
    std::string Name() const
    {
        std::lock_guard<std::recursive_mutex> lock(m_RunMutex);
        return m_Name;
    }
    std::string BaseName() const
    {
        std::lock_guard<std::recursive_mutex> lock(m_RunMutex);
        return m_Basename;
    }
    std::string GetCodeHash() const
    {
        std::lock_guard<std::recursive_mutex> lock(m_RunMutex);
        return m_CodeVersionHash;
    }
    std::string GetRuntimeLanguage() const { return RuntimeLanguage(); }
    bool RequiresRootSerialization() const { return UsesAnalysisManagers() || RuntimeLanguage() == "python"; }
    ExecutionContext &GetExecutionContext() { return m_Context; }
    const ExecutionContext &GetExecutionContext() const { return m_Context; }
    void SetCacheDirectory(const std::string &path)
    {
        std::lock_guard<std::recursive_mutex> lock(m_RunMutex);
        m_Context.SetCacheDirectory(path);
    }
    void SetOutputDirectory(const std::string &path)
    {
        std::lock_guard<std::recursive_mutex> lock(m_RunMutex);
        m_Context.SetOutputDirectory(path);
    }
    std::string GetCacheDirectory() const { return m_Context.CacheDirectory().string(); }
    std::string GetOutputDirectory() const { return m_Context.OutputDirectory().string(); }
    std::string GetRunId() const { return m_Context.RunId(); }
    std::string GetLastProvenancePath() const
    {
        auto manifest = ProvenanceRecorder::FindModuleRun(m_Context.RunId());
        if (!manifest) manifest = ProvenanceRecorder::FindLastModuleRun(Name());
        return manifest ? manifest->ManifestPath : std::string();
    }
    std::string GetLastProvenanceJSON(int indent = 2) const
    {
        auto manifest = ProvenanceRecorder::FindModuleRun(m_Context.RunId());
        if (!manifest) manifest = ProvenanceRecorder::FindLastModuleRun(Name());
        return manifest ? manifest->ToJSON(indent) : std::string();
    }

    void SetParamValue(const std::string &key, const ParamValue &value)
    {
        std::lock_guard<std::recursive_mutex> lock(m_RunMutex);
        m_Param.SetParamVariant(key, value);
    }
    ParamValue GetParamValue(const std::string &key) const
    {
        std::lock_guard<std::recursive_mutex> lock(m_RunMutex);
        return m_Param.Get<ParamValue>(key);
    }
    bool HasParam(const std::string &key) const
    {
        std::lock_guard<std::recursive_mutex> lock(m_RunMutex);
        return m_Param.Has(key);
    }
    void LoadParamsFromYAML(const std::string &path)
    {
        std::lock_guard<std::recursive_mutex> lock(m_RunMutex);
        m_Param.LoadYAMLFile(path);
    }
    void LoadParamsFromJSON(const std::string &path)
    {
        std::lock_guard<std::recursive_mutex> lock(m_RunMutex);
        m_Param.LoadJSONFile(path);
    }
    void SetParamsFromJSON(const std::string &document)
    {
        std::lock_guard<std::recursive_mutex> lock(m_RunMutex);
        m_Param.SetParamsFromJSON(document);
    }
    void SaveParamsToYAML(const std::string &path)
    {
        std::lock_guard<std::recursive_mutex> lock(m_RunMutex);
        m_Param.SaveYAMLFile(path);
    }
    void SaveParamsToJSON(const std::string &path)
    {
        std::lock_guard<std::recursive_mutex> lock(m_RunMutex);
        m_Param.SaveJSONFile(path);
    }
    std::string DumpParamsToYAML(int indent = 2)
    {
        std::lock_guard<std::recursive_mutex> lock(m_RunMutex);
        return m_Param.DumpYAML(indent);
    }
    std::string DumpParamsToJSON(int indent = 4)
    {
        std::lock_guard<std::recursive_mutex> lock(m_RunMutex);
        return m_Param.DumpJSON(indent);
    }

    std::string GetStatus() const { return ToString(m_Status.load()); }
    ModuleStatus GetStatusEnum() const { return m_Status.load(); }
    RunResult GetLastRunResult() const
    {
        std::lock_guard<std::mutex> lock(m_ResultMutex);
        return m_LastResult;
    }
    ParamManager &GetParamManager() { return m_Param; }
    const ParamManager &GetParamManager() const { return m_Param; }
    std::map<std::string, double> GetProgressSnapshot() const
    {
        std::lock_guard<std::mutex> lock(m_ManagerMutex);
        std::map<std::string, double> result;
        for (const auto &[name, manager] : m_Managers)
            result[name] = manager->GetProgress();
        return result;
    }
    void SetStatus(ModuleStatus status)
    {
        m_Status.store(status);
        LOG_INFO(Name(), "Status : " << ToString(status));
    }

  protected:
    virtual void Init() = 0;
    virtual void Execute() = 0;
    virtual void Finalize() = 0;
    virtual void OnFailure(ModulePhase, const std::string &) {}
    virtual bool UsesAnalysisManagers() const { return true; }
    virtual std::string RuntimeLanguage() const { return "cpp"; }
    virtual void ConfigureProvenance() { ProvenanceRecorder::SetPluginOrigin(m_Context.RunId(), m_PluginOrigin); }
    virtual std::string AnalysisSnapshotState() const
    {
        std::lock_guard<std::mutex> lock(m_ManagerMutex);
        nlohmann::json state = nlohmann::json::array();
        for (const auto &[name, manager] : m_Managers)
            state.push_back({{"name", name}, {"state", nlohmann::json::parse(manager->SnapshotState())}});
        return state.dump();
    }

    void RegisterAnalysisManager(const std::string &name = "main")
    {
        std::lock_guard<std::mutex> lock(m_ManagerMutex);
        auto it = m_Managers.find(name);
        if (it != m_Managers.end())
            throw std::runtime_error("Analysis manager already exists: " + name);
        else
            m_Managers[name] = std::make_unique<AnalysisManager>();
    }

    AnalysisManager *GetAnalysisManager(const std::string &name) const
    {
        std::lock_guard<std::mutex> lock(m_ManagerMutex);
        auto it = m_Managers.find(name);
        return it != m_Managers.end() ? it->second.get() : nullptr;
    }

    AnalysisManager *Am(const std::string &name = "main") const { return GetAnalysisManager(name); }
    std::filesystem::path StageOutput(const std::filesystem::path &path) { return m_Context.StageOutput(path); }
    std::filesystem::path FinalOutput(const std::filesystem::path &path) const { return m_Context.FinalOutput(path); }
    void TrackInput(const std::filesystem::path &path) { ProvenanceRecorder::TrackInput(m_Context.RunId(), path); }
    ExecutionContext &Context() { return m_Context; }
    const ExecutionContext &Context() const { return m_Context; }

    ParamManager m_Param;
    std::string m_Hash;

    std::string m_Basename = "Interface";
    std::string m_Name = "";
    std::string m_CodeVersionHash = "";

  private:
    std::map<std::string, std::unique_ptr<AnalysisManager>> m_Managers;
    mutable std::mutex m_ManagerMutex;
    std::atomic<ModuleStatus> m_Status{ModuleStatus::Pending};
    mutable std::mutex m_ResultMutex;
    mutable std::recursive_mutex m_RunMutex;
    RunResult m_LastResult;
    ExecutionContext m_Context;
    bool m_ExternalRunReserved = false;
    std::optional<PluginOrigin> m_PluginOrigin;
    std::string m_CacheDecision = "not_checked";
    std::string m_CacheReason;

    struct CheckDecision
    {
        bool ShouldRun = true;
        std::string Message;
    };

    RunResult Finish_(ModuleStatus status, ModulePhase phase, std::string message, std::exception_ptr exception = nullptr)
    {
        if (status != ModuleStatus::Done && m_Context.IsActive()) m_Context.RollbackRun();
        SetStatus(status);
        RunResult result{status, phase, std::move(message), std::move(exception)};
        result.CacheDecision = m_CacheDecision;
        result.CacheReason = m_CacheReason;
        {
            std::lock_guard<std::mutex> lock(m_ResultMutex);
            m_LastResult = result;
        }
        FinalizeProvenance_(result);
        return result;
    }

    void FinalizeProvenance_(const RunResult &result) noexcept
    {
        try
        {
            if (ProvenanceRecorder::FindModuleRun(m_Context.RunId())) return;
            const std::string expectedPath =
                result.Status == ModuleStatus::Done
                    ? ProvenanceRecorder::SuccessfulModuleManifestPath(m_Context.OutputDirectory(), m_Context.RunId())
                    : ProvenanceRecorder::TerminalModuleManifestPath(m_Context.CacheDirectory(), m_Context.RunId());
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
                m_Context.RunId(), GetMetadata(), m_CodeVersionHash, m_Hash, m_Param.DumpJSON(),
                m_Context.OutputDirectory(), m_Context.CacheDirectory(), result, {}, expectedPath);
            ProvenanceRecorder::WriteModuleRun(manifest, expectedPath);
            ProvenanceRecorder::StoreModuleRun(manifest);
        }
        catch (const std::exception &error)
        {
            ProvenanceRecorder::DiscardModuleRun(m_Context.RunId());
            LOG_ERROR(Name(), "Failed to record provenance: " << error.what());
        }
        catch (...)
        {
            ProvenanceRecorder::DiscardModuleRun(m_Context.RunId());
            LOG_ERROR(Name(), "Failed to record provenance with an unknown exception");
        }
    }

    RunResult Fail_(ModulePhase phase, const std::string &message, std::exception_ptr exception)
    {
        LOG_ERROR(Name(), "Module failed during " << ToString(phase) << ": " << message);
        InvokeFailureHook_(phase, message);
        return Finish_(ModuleStatus::Failed, phase, message, std::move(exception));
    }

    void InvokeFailureHook_(ModulePhase phase, const std::string &message)
    {
        try
        {
            OnFailure(phase, message);
        }
        catch (const std::exception &e)
        {
            LOG_ERROR(Name(), "OnFailure hook failed: " << e.what());
        }
        catch (...)
        {
            LOG_ERROR(Name(), "OnFailure hook failed with an unknown exception");
        }
    }

    std::string ComputeSnapshotHash_() const
    {
        const std::string artifactHash = m_PluginOrigin ? m_PluginOrigin->ArtifactSha256 : std::string();
        return SnapshotHasher::ComputeSerialized(m_Param, m_Basename, m_CodeVersionHash,
                                                 AnalysisSnapshotState(), m_Context.SnapshotState(), artifactHash,
                                                 ProvenanceRecorder::InputSnapshotState(m_Context.RunId()));
    }

    CheckDecision RunCheck_()
    {
        if (m_Param.Get<bool>("dry_run"))
        {
            m_CacheDecision = "not_checked";
            m_CacheReason = "dry_run enabled";
            LOG_INFO(Name(), "DRY run is enabled. variables and setting will be shown.");
            for (auto &[_, mg] : m_Managers)
            {
                mg->PrintConfigSummary();
                mg->PrintHistogramSummary();
                mg->PrintCutSummary();
            }
            LOG_INFO("ParamManager", m_Param.DumpJSON());
            return {false, "dry_run enabled"};
        }
        m_CacheDecision = "checking";
        m_CacheReason = "evaluating snapshot cache";
        m_Hash = ComputeSnapshotHash_();
        if (m_Param.Get<bool>("force_run"))
        {
            m_CacheDecision = "bypassed";
            m_CacheReason = "force_run enabled";
            LOG_INFO(Name(), "Force run is enabled. Run will be started.");
            return {true, ""};
        }

        auto cached = CacheManager::Lookup(m_Basename, m_Hash, m_Context.CacheDirectory().string());
        if (!cached)
        {
            m_CacheDecision = "miss";
            m_CacheReason = "snapshot not found";
        }
        if (cached)
        {
            std::string reason;
            if (!ProvenanceRecorder::ValidateCachedRun(cached->Provenance, m_Hash, m_Context.OutputDirectory(), &reason))
            {
                LOG_WARN(Name(), "Discarding stale snapshot cache entry: " << reason);
                m_CacheDecision = "miss";
                m_CacheReason = reason;
                CacheManager::RemoveHash(m_Basename, m_Hash, m_Context.CacheDirectory().string());
                cached.reset();
            }
        }
        if (cached)
        {
            m_CacheDecision = "hit";
            m_CacheReason = "snapshot and recorded outputs matched";
            ProvenanceRecorder::SetCacheSource(m_Context.RunId(), cached->Provenance);
            LOG_INFO(Name(), "Matching snapshot is already cached.");
        }
        return {!cached, cached ? "snapshot already cached" : ""};
    }
};
