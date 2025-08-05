#pragma once
#include "AnalysisManager.hh"
#include "IAnalysisModule.hh"
#include <iostream>
#include <memory>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;

class RDFHistModule : public IAnalysisModule
{
  public:
    RDFHistModule()
    {
        basename = "@BASENAME@";
        code_version_hash = "@VERSION_HASH@";

        _param.Register<std::string>("config", "config_gen.yaml", "configuration file");
        _param.Register<std::string>("hist_config", "hist.yaml", "hist definition file");
        _param.Register<std::string>("output", "hists.root", "histogram output");
        _param.Register<std::string>("type", "0", "0-9");
        _param.Register<std::string>("prefix", "signal", "Prefix");
    }

    void Description() const override { LOG_INFO(m_name, "An instance of " << basename << ", which is RDF based Histogram filler"); }

    void Init() override
    {
        mg = GetAnalysisManager("main");
        mg->SetRDFInputFromConfig(_param.Get<std::string>("config"));
        mg->ApplyRDFFilter("types", ("Type==" + _param.Get<std::string>("type")));
        mg->BookRDFHistsFromConfig(_param.Get<std::string>("hist_config"), _param.Get<std::string>("prefix"));
    }

    void Execute() override { mg->SaveHistsRDF(_param.Get<std::string>("output")); }

    void Finalize() override { mg->WriteMetaData(_param.Get<std::string>("output"), _hash, basename, _param.DumpJSON()); }

  private:
    AnalysisManager *mg;
};
