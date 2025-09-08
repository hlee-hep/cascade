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
    std::shared_ptr<IAnalysisModule> RegisterModule(const std::string &base, const std::string &instance_name);
    std::vector<std::string> ListRegisteredModules() const;
    std::vector<std::string> ListAvailableModules() const { return AnalysisModuleRegistry::Get().ListModules(); }

    std::shared_ptr<IAnalysisModule> GetModule(const std::string &name);
    std::string GetStatus(const std::string &name) const;
    std::map<std::string, std::map<std::string, double>> GetAllProgress() const;
    void SaveRunLog() const;

    void RunAModule(std::shared_ptr<IAnalysisModule> mod);
    void RunAModule(const std::string &name);
    void SequentialRun();
    void RunModules(const std::vector<std::string> &group);
    void RunModules(std::vector<std::shared_ptr<IAnalysisModule>> group);
    DAGManager &GetDAGManager() { return *dag; }
    void RunDAG();

  private:
    std::map<std::string, std::shared_ptr<IAnalysisModule>> modules_;
    std::unique_ptr<DAGManager> dag;

    std::vector<std::shared_ptr<IAnalysisModule>> executed_modules_;

    std::map<std::string, int> module_name_counter_;

    std::atomic<bool> execution_done = false;
    mutable std::mutex status_mutex;

    std::string _runid;
};