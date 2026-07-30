#pragma once
#include "AnalysisManager.hh"
#include "AnalysisModuleRegistry.hh"
#include "DAGManager.hh"
#include "IAnalysisModule.hh"
#include "ModuleRun.hh"
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class AMCM
{
  public:
    AMCM();

    std::shared_ptr<IAnalysisModule> RegisterModule(const std::string &base);
    std::shared_ptr<IAnalysisModule> RegisterModule(const std::string &base, const std::string &instanceName);
    std::vector<std::string> ListRegisteredModules() const;
    std::vector<std::string> ListAvailableModules() const { return AnalysisModuleRegistry::Get().ListModules(); }
    std::vector<ModuleMetadata> ListAvailableModuleMetadata() const { return AnalysisModuleRegistry::Get().ListModuleMetadata(); }

    std::shared_ptr<IAnalysisModule> GetModule(const std::string &name);
    std::string GetStatus(const std::string &name) const;
    std::map<std::string, std::map<std::string, double>> GetAllProgress() const;
    void SaveRunLog() const;

    RunResult RunAModule(std::shared_ptr<IAnalysisModule> mod);
    RunResult RunAModule(const std::string &name);
    RunResult RunAModuleIsolated(std::shared_ptr<IAnalysisModule> mod);
    RunResult RunAModuleIsolated(const std::string &name);
    std::vector<RunResult> SequentialRun(bool failFast = true);
    std::vector<RunResult> RunModules(const std::vector<std::string> &group, bool failFast = true);
    std::vector<RunResult> RunModules(std::vector<std::shared_ptr<IAnalysisModule>> group, bool failFast = true);
    DAGManager &GetDAGManager() { return *m_Dag; }
    void AddModuleToDAG(const std::string &name, const std::vector<std::string> &dependencies, bool isolated = false);
    void LinkDAGModuleParameter(const std::string &fromNode, const std::string &fromKey, const std::string &toNode,
                                const std::string &toKey);
    DAGRunResult RunDAG(bool failFast = true);
    void LoadPlugins(const std::string &path);

  private:
    std::map<std::string, std::shared_ptr<IAnalysisModule>> m_Modules;
    std::unique_ptr<DAGManager> m_Dag;

    struct RunLogEntry
    {
        std::string Name;
        std::string BaseName;
        std::string CodeHash;
        std::string ParamsYaml;
        RunResult Result;
    };
    std::vector<RunLogEntry> m_ExecutedModules;

    std::map<std::string, int> m_ModuleNameCounter;

    std::recursive_mutex m_RegistrationMutex;
    mutable std::mutex m_ControlMutex;

    void RecordRun_(const std::shared_ptr<IAnalysisModule> &module, const RunResult &result);
    std::shared_ptr<IAnalysisModule> RegisteredModule_(const std::string &name) const;
    std::shared_ptr<IAnalysisModule> ValidateModuleHandle_(const std::shared_ptr<IAnalysisModule> &module) const;
};
