#pragma once
#include "AnalysisManager.hh"
#include "IAnalysisModule.hh"
#include <iostream>
#include <memory>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;

class HistModule : public IAnalysisModule
{
  public:
    HistModule()
    {
        m_Basename = "@BASENAME@";
        m_CodeVersionHash = "@VERSION_HASH@";

        m_Param.Register<std::string>("config", "config_gen.yaml", "configuration file");
        m_Param.Register<std::string>("cut_config", "cut.yaml", "cut list file");
        m_Param.Register<std::string>("hist_config", "hist.yaml", "hist definition file");
        m_Param.Register<std::string>("output", "hists.root", "histogram output");
    }

    void Description() const override { LOG_INFO(m_Name, "An instance of " << m_Basename << ", which is old-style Histogram filler"); }

    void Init() override
    {
        m_Manager = GetAnalysisManager("main");
        m_Manager->LoadConfig(m_Param.Get<std::string>("config"));
        m_Manager->InitTree();
        m_Manager->LoadCuts(m_Param.Get<std::string>("cut_config"));
        m_Manager->ActivateCuts();
        m_Manager->PrintCuts();
        m_Manager->InitNewHistsFromConfig(m_Param.Get<std::string>("hist_config"), "1");
    }

    void Execute() override
    {
        for (Long64_t i = 0; i < m_Manager->GetEntries(); i++)
        {
            m_Manager->LoadEntry(i);
            if (!m_Manager->PassCut()) continue;
            m_Manager->FillHists(1.0);
        }
    }
    void Finalize() override { m_Manager->SaveHists(m_Param.Get<std::string>("output")); }

  private:
    AnalysisManager *m_Manager;

  protected:
};
