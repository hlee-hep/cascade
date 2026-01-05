#pragma once
#include "AnalysisManager.hh"
#include "AnalysisModuleRegistry.hh"
#include "DAGManager.hh"
#include "IAnalysisModule.hh"
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <string>
#include <vector>

namespace py = pybind11;

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

    void RunAModule(std::shared_ptr<IAnalysisModule> mod);
    void RunAModule(const std::string &name);
    void SequentialRun();
    void RunModules(const std::vector<std::string> &group);
    void RunModules(std::vector<std::shared_ptr<IAnalysisModule>> group);
    DAGManager &GetDAGManager() { return *m_Dag; }
    void RunDAG();
    void LoadPlugins(const std::string &path);

  private:
    std::map<std::string, std::shared_ptr<IAnalysisModule>> m_Modules;
    std::unique_ptr<DAGManager> m_Dag;

    std::vector<std::shared_ptr<IAnalysisModule>> m_ExecutedModules;

    std::map<std::string, int> m_ModuleNameCounter;

    std::atomic<bool> m_ExecutionDone = false;
    mutable std::mutex m_StatusMutex;

    std::string m_RunId;
};
