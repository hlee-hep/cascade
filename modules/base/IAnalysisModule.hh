#pragma once
#include "AnalysisManager.hh"
#include "CacheManager.hh"
#include "ExecutionContext.hh"
#include "InterruptManager.hh"
#include "Logger.hh"
#include "ModuleMetadata.hh"
#include "ModuleRun.hh"
#include "ParamManager.hh"
#include "SnapshotHasher.hh"
#include "sha256.hh"
#include "Version.hh"
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
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

    void PrepareExternalRun()
    {
        std::lock_guard<std::recursive_mutex> runLock(m_RunMutex);
        if (m_ExternalRunReserved || m_Context.IsActive()) throw std::runtime_error("Module run is already active: " + Name());
        m_Context.BeginRun(Name(), BaseName());
        m_ExternalRunReserved = true;
        SetStatus(ModuleStatus::Initializing);
    }

    RunResult RunPreparedExternal() { return RunImpl_(true); }

    RunResult AdoptExternalRunResult(RunResult result)
    {
        std::lock_guard<std::recursive_mutex> runLock(m_RunMutex);
        if (!m_ExternalRunReserved) throw std::runtime_error("Module has no reserved external run: " + Name());
        m_ExternalRunReserved = false;
        m_Context.CleanupExternalRun();
        if (result.Failed() && !result.Exception)
            result.Exception = std::make_exception_ptr(std::runtime_error(result.Message.empty() ? "Isolated module failed" : result.Message));
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
        if (externalPrepared) m_ExternalRunReserved = false;
        {
            std::lock_guard<std::mutex> managerLock(m_ManagerMutex);
            m_Managers.clear();
        }
        RegisterAnalysisManager("main");
        SetStatus(ModuleStatus::Initializing);
        try
        {
            if (!m_Context.IsActive()) m_Context.BeginRun(Name(), BaseName());
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
            m_Context.Outputs().Commit();
            CacheManager::AddHash(m_Basename, m_Hash, m_Context.CacheDirectory().string());
            try
            {
                m_Context.CompleteRun();
            }
            catch (...)
            {
                CacheManager::RemoveHash(m_Basename, m_Hash, m_Context.CacheDirectory().string());
                throw;
            }
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
        info.Name = m_Basename;
        info.Version = CascadeVersionString();
        return info;
    }

    std::string GetParamsToJSON()
    {
        std::lock_guard<std::recursive_mutex> lock(m_RunMutex);
        return m_Param.DumpJSON();
    }

    void SetName(const std::string &name) { m_Name = name; }
    std::string Name() const { return m_Name; }
    std::string BaseName() const { return m_Basename; }
    std::string GetCodeHash() const { return m_CodeVersionHash; }
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
        std::lock_guard<std::mutex> lock(m_ResultMutex);
        m_LastResult = result;
        return result;
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
        std::lock_guard<std::mutex> lock(m_ManagerMutex);
        return SnapshotHasher::Compute(m_Param, m_Managers, m_Basename, m_CodeVersionHash, m_Context.SnapshotState());
    }

    CheckDecision RunCheck_()
    {
        if (m_Param.Get<bool>("dry_run"))
        {
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
        m_Hash = ComputeSnapshotHash_();
        if (m_Param.Get<bool>("force_run"))
        {
            LOG_INFO(Name(), "Force run is enabled. Run will be started.");
            return {true, ""};
        }

        bool dupl = CacheManager::IsHashCached(m_Basename, m_Hash, m_Context.CacheDirectory().string());
        if (dupl) LOG_INFO(Name(), "Matching snapshot is already cached.");
        return {!dupl, dupl ? "snapshot already cached" : ""};
    }
};
