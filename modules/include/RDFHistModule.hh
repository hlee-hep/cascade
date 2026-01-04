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
        m_Basename = "@BASENAME@";
        m_CodeVersionHash = "@VERSION_HASH@";

        m_Param.Register<std::string>("config", "config_gen.yaml", "configuration file");
        m_Param.Register<std::string>("hist_config", "hist.yaml", "hist definition file");
        m_Param.Register<std::string>("output", "hists.root", "histogram output");
        m_Param.Register<std::string>("type", "0", "0-9");
        m_Param.Register<std::string>("prefix", "signal", "Prefix");
    }

    void Description() const override { LOG_INFO(m_Name, "An instance of " << m_Basename << ", which is RDF based Histogram filler"); }

    void Init() override
    {
        m_Manager = GetAnalysisManager("main");
        m_Manager->InitRdfFromConfig(m_Param.Get<std::string>("config"));
        m_Manager->ApplyRdfFilter("types", ("Type==" + m_Param.Get<std::string>("type")));
        m_Manager->BookRdfHistogramsFromConfig(m_Param.Get<std::string>("hist_config"), m_Param.Get<std::string>("prefix"));
    }

    void Execute() override { m_Manager->WriteRdfHistograms(m_Param.Get<std::string>("output")); }

    void Finalize() override { m_Manager->WriteMetadata(m_Param.Get<std::string>("output"), m_Hash, m_Basename, m_Param.DumpJSON()); }

  private:
    AnalysisManager *m_Manager;
};
