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
        m_Basename = "@BASENAME@";
        m_CodeVersionHash = "@VERSION_HASH@";

        m_Param.Register<std::string>("config", "config_gen.yaml", "configuration file");
        m_Param.Register<std::string>("output", "skim.root", "root output");
    }

    void Description() const override { LOG_INFO(m_Name, "An instance of " << m_Basename << ", which is RDF based Skimmer"); }

    void Init() override
    {
        m_Manager = GetAnalysisManager("main");
        m_Manager->SetRDFInputFromConfig(m_Param.Get<std::string>("config"));
    }
    void Execute() override { LOG_INFO(m_Name, "Not implemented yet!"); }
    void Finalize() override { m_Manager->SnapshotRDF("test", m_Param.Get<std::string>("output"), TreeOpt::Om::Append); }

  private:
    AnalysisManager *m_Manager;
};
