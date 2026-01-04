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
        m_Basename = "@BASENAME@";
        m_CodeVersionHash = "@VERSION_HASH@";

        m_Param.Register<std::string>("input", "skim.root", "root input");
        m_Param.Register<std::string>("input_hist", "hists.root", "root input");
    }

    void Description() const override { LOG_INFO(m_Name, "An instance of " << m_Basename << ", which is DAG Debug module"); }

    void Init() override
    {
        m_Manager = GetAnalysisManager("main");
        LOG_INFO(m_Name, m_Param.Get<std::string>("input_hist"));
        TFile *file = TFile::Open(m_Param.Get<std::string>("input").c_str());
        if (!file)
            LOG_ERROR(m_Name, "No file found...");
        else
            LOG_INFO(m_Name, "DAGTest is completed." << file->GetName());
    }
    void Execute() override {}
    void Finalize() override {}

  private:
    AnalysisManager *m_Manager;
};
