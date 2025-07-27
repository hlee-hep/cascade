#pragma once
#include "AnalysisManager.hh"
#include "IAnalysisModule.hh"
#include <iostream>
#include <memory>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;

class RDFSkimModule : public IAnalysisModule
{
  public:
    RDFSkimModule()
    {
        basename = "@BASENAME@";
        code_version_hash = "@VERSION_HASH@";

        RegisterAnalysisManager("main");
        _param.Register<std::string>("config", "config_gen.yaml",
                                     "configuration file");
        _param.Register<std::string>("output", "skim.root", "root output");
    }

    void Description() const override
    {
        LOG_INFO(m_name, "An instance of " << basename
                                           << ", which is RDF based Skimmer");
    }

    void Init() override
    {
        mg = GetAnalysisManager("main");
        mg->SetRDFInputFromConfig(_param.Get<std::string>("config"));
    }
    void Execute() override
    {
        SetStatus("Running");
        LOG_INFO(m_name, "Not implemented yet!");
    }
    void Finalize() override
    {
        mg->SnapshotRDF("test", _param.Get<std::string>("output"),
                        TreeOpt::OM::kAppend);
        SetStatus("Done");
    }

    std::string ComputeSnapshotHash() const override
    {
        return SnapshotHasher::Compute(_param, GetAllManagers(), basename,
                                       code_version_hash);
    }

  private:
    AnalysisManager *mg;
};
