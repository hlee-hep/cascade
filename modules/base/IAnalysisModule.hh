#pragma once

#include "ExecutionContext.hh"
#include "ModuleMetadata.hh"
#include "ModuleRun.hh"
#include "ParamManager.hh"
#include "PluginTrust.hh"

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>

class AnalysisManager;

class IAnalysisModule
{
  public:
    IAnalysisModule();
    virtual ~IAnalysisModule();

    IAnalysisModule(const IAnalysisModule &) = delete;
    IAnalysisModule &operator=(const IAnalysisModule &) = delete;
    IAnalysisModule(IAnalysisModule &&) = delete;
    IAnalysisModule &operator=(IAnalysisModule &&) = delete;

    RunResult Run();
    void PrepareExternalRun();
    void PrepareExternalRunWithId(const std::string &runId);
    RunResult RunPreparedExternal();
    RunResult AdoptExternalRunResult(RunResult result);

    void RequestCancellation();
    bool IsCancellationRequested() const;

    virtual void Description() const = 0;
    virtual ModuleMetadata GetMetadata() const;

    std::string GetParamsToJSON();
    void SetName(const std::string &name);
    void SetBaseName(const std::string &name);
    void SetCodeHash(const std::string &hash);
    void SetPluginOrigin(std::optional<PluginOrigin> origin);
    std::optional<PluginOrigin> GetPluginOrigin() const;
    std::string Name() const;
    std::string BaseName() const;
    std::string GetCodeHash() const;
    std::string GetRuntimeLanguage() const;
    bool RequiresRootSerialization() const;

    ExecutionContext &GetExecutionContext();
    const ExecutionContext &GetExecutionContext() const;
    void SetCacheDirectory(const std::string &path);
    void SetOutputDirectory(const std::string &path);
    std::string GetCacheDirectory() const;
    std::string GetOutputDirectory() const;
    std::string GetRunId() const;
    std::string GetLastProvenancePath() const;
    std::string GetLastProvenanceJSON(int indent = 2) const;

    void SetParamValue(const std::string &key, const ParamValue &value);
    ParamValue GetParamValue(const std::string &key) const;
    bool HasParam(const std::string &key) const;
    void LoadParamsFromYAML(const std::string &path);
    void LoadParamsFromJSON(const std::string &path);
    void SetParamsFromJSON(const std::string &document);
    void SaveParamsToYAML(const std::string &path);
    void SaveParamsToJSON(const std::string &path);
    std::string DumpParamsToYAML(int indent = 2);
    std::string DumpParamsToJSON(int indent = 4);

    std::string GetStatus() const;
    ModuleStatus GetStatusEnum() const;
    RunResult GetLastRunResult() const;
    ParamManager &GetParamManager();
    const ParamManager &GetParamManager() const;
    std::map<std::string, double> GetProgressSnapshot() const;
    void SetStatus(ModuleStatus status);

  protected:
    virtual void Init() = 0;
    virtual void Execute() = 0;
    virtual void Finalize() = 0;
    virtual void OnFailure(ModulePhase, const std::string &) {}
    virtual bool UsesAnalysisManagers() const { return true; }
    virtual std::string RuntimeLanguage() const { return "cpp"; }
    virtual void ConfigureProvenance();
    virtual std::string AnalysisSnapshotState() const;

    ParamManager &Parameters();
    const ParamManager &Parameters() const;
    void RegisterAnalysisManager(const std::string &name = "main");
    AnalysisManager *GetAnalysisManager(const std::string &name) const;
    AnalysisManager *Am(const std::string &name = "main") const;
    std::filesystem::path StageOutput(const std::filesystem::path &path);
    std::filesystem::path FinalOutput(const std::filesystem::path &path) const;
    void TrackInput(const std::filesystem::path &path);
    ExecutionContext &Context();
    const ExecutionContext &Context() const;

  private:
    struct Impl;
    struct CheckDecision;
    std::unique_ptr<Impl> m_Impl;

    RunResult RunImpl_(bool externalPrepared);
    RunResult Finish_(ModuleStatus status, ModulePhase phase, std::string message,
                      std::exception_ptr exception = nullptr);
    void FinalizeProvenance_(const RunResult &result) noexcept;
    RunResult Fail_(ModulePhase phase, const std::string &message, std::exception_ptr exception);
    void InvokeFailureHook_(ModulePhase phase, const std::string &message);
    std::string ComputeSnapshotHash_() const;
    CheckDecision RunCheck_();
};
