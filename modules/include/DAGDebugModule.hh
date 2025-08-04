#pragma once
#include "AnalysisManager.hh"
#include "IAnalysisModule.hh"
#include <iostream>
#include <memory>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;

class DAGDebugModule : public IAnalysisModule
{
  public:
    DAGDebugModule()
    {
        basename = "@BASENAME@";
        code_version_hash = "@VERSION_HASH@";

        RegisterAnalysisManager("main");
        _param.Register<std::string>("input", "skim.root", "root input");
        _param.Register<std::string>("input_hist", "hists.root", "root input");
    }

    void Description() const override { LOG_INFO(m_name, "An instance of " << basename << ", which is DAG Debug module"); }

    void Init() override
    {
        SetStatus("Running");
        mg = GetAnalysisManager("main");
        LOG_INFO(m_name, _param.Get<std::string>("input_hist"));
        TFile *file = TFile::Open(_param.Get<std::string>("input").c_str());
        if (!file)
            LOG_ERROR(m_name, "No file found...");
        else
            LOG_INFO(m_name, "DAGTest is completed." << file->GetName());
    }
    void Execute() override {}
    void Finalize() override { SetStatus("Done"); }

    std::string ComputeSnapshotHash() const override { return SnapshotHasher::Compute(_param, GetAllManagers(), basename, code_version_hash); }

  private:
    AnalysisManager *mg;
};
