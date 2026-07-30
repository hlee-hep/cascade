#include "RootEventModule.hh"

#include "Logger.hh"

#include <TFile.h>
#include <TTree.h>

#include <algorithm>
#include <fstream>

RootEventModule::RootEventModule()
{
    m_Basename = "@BASENAME@";
    m_CodeVersionHash = "@VERSION_HASH@";
    m_Param.Register<std::string>("output", "events.root", "ROOT output path relative to the execution output directory");
    m_Param.Register<std::string>("manifest", "events_manifest.json", "Portable metadata consumed when PyROOT is unavailable");
    m_Param.Register<int>("events", 100, "Number of generated events");
    m_Param.Register<double>("scale", 0.5, "Scale applied to the generated value");
}

void RootEventModule::Description() const
{
    LOG_INFO(m_Basename, "Generates a ROOT TTree through Cascade's output transaction.");
}

void RootEventModule::Init()
{
    if (m_Param.Get<int>("events") < 1) throw std::invalid_argument("events must be positive");
}

void RootEventModule::Execute()
{
    const auto staged = StageOutput(m_Param.Get<std::string>("output"));
    TFile output(staged.c_str(), "RECREATE");
    if (output.IsZombie()) throw std::runtime_error("cannot create staged ROOT output");

    TTree tree("events", "Generated example events");
    int event = 0;
    double value = 0.0;
    tree.Branch("event", &event);
    tree.Branch("value", &value);
    for (event = 0; event < m_Param.Get<int>("events"); ++event)
    {
        value = event * m_Param.Get<double>("scale");
        tree.Fill();
    }
    tree.Write();
    output.Close();

    const int events = m_Param.Get<int>("events");
    const double scale = m_Param.Get<double>("scale");
    const double first = 0.0;
    const double last = (events - 1) * scale;
    std::ofstream manifest(StageOutput(m_Param.Get<std::string>("manifest")));
    if (!manifest) throw std::runtime_error("cannot create staged event manifest");
    manifest << "{\n"
             << "  \"entries\": " << events << ",\n"
             << "  \"minimum\": " << std::min(first, last) << ",\n"
             << "  \"maximum\": " << std::max(first, last) << ",\n"
             << "  \"mean\": " << (first + last) / 2.0 << "\n"
             << "}\n";
}

void RootEventModule::Finalize() {}
