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
        basename = "@BASENAME@";
        code_version_hash = "@VERSION_HASH@";

        _param.Register<std::string>("config", "config_gen.yaml", "configuration file");
        _param.Register<std::string>("cut_config", "cut.yaml", "cut list file");
        _param.Register<std::string>("hist_config", "hist.yaml", "hist definition file");
        _param.Register<std::string>("output", "hists.root", "histogram output");
    }

    void Description() const override { LOG_INFO(m_name, "An instance of " << basename << ", which is old-style Histogram filler"); }

    void Init() override
    {
        mg = GetAnalysisManager("main");
        mg->LoadConfig(_param.Get<std::string>("config"));
        mg->InitTree();
        mg->LoadCuts(_param.Get<std::string>("cut_config"));
        mg->ActivateCuts();
        mg->PrintCuts();
        mg->InitNewHistsFromConfig(_param.Get<std::string>("hist_config"), "1");
    }

    void Execute() override
    {
        for (Long64_t i = 0; i < mg->GetEntries(); i++)
        {
            mg->LoadEntry(i);
            if (!mg->PassCut()) continue;
            mg->FillHists(1.0);
        }
    }
    void Finalize() override { mg->SaveHists(_param.Get<std::string>("output")); }

  private:
    AnalysisManager *mg;

  protected:
};
