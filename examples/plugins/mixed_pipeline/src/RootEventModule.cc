#include "RootEventModule.hh"

#include "Logger.hh"

#include <TFile.h>
#include <TTree.h>

#include <algorithm>
#include <fstream>

RootEventModule::RootEventModule()
{
    SetBaseName("@BASENAME@");
    SetCodeHash("@VERSION_HASH@");
    Parameters().Register<std::string>("output", "events.root", "ROOT output path relative to the execution output directory");
    Parameters().Register<std::string>("manifest", "events_manifest.json", "Portable metadata consumed when PyROOT is unavailable");
    Parameters().Register<int>("events", 100, "Number of generated events");
    Parameters().Register<double>("scale", 0.5, "Scale applied to the generated value");
}

void RootEventModule::Description() const
{
    LOG_INFO(BaseName(), "Generates a ROOT TTree through Cascade's output transaction.");
}

void RootEventModule::Init()
{
    if (Parameters().Get<int>("events") < 1) throw std::invalid_argument("events must be positive");
}

void RootEventModule::Execute()
{
    const auto staged = StageOutput(Parameters().Get<std::string>("output"));
    TFile output(staged.c_str(), "RECREATE");
    if (output.IsZombie()) throw std::runtime_error("cannot create staged ROOT output");

    TTree tree("events", "Generated example events");
    int event = 0;
    double value = 0.0;
    tree.Branch("event", &event);
    tree.Branch("value", &value);
    for (event = 0; event < Parameters().Get<int>("events"); ++event)
    {
        value = event * Parameters().Get<double>("scale");
        tree.Fill();
    }
    tree.Write();
    output.Close();

    const int events = Parameters().Get<int>("events");
    const double scale = Parameters().Get<double>("scale");
    const double first = 0.0;
    const double last = (events - 1) * scale;
    std::ofstream manifest(StageOutput(Parameters().Get<std::string>("manifest")));
    if (!manifest) throw std::runtime_error("cannot create staged event manifest");
    manifest << "{\n"
             << "  \"entries\": " << events << ",\n"
             << "  \"minimum\": " << std::min(first, last) << ",\n"
             << "  \"maximum\": " << std::max(first, last) << ",\n"
             << "  \"mean\": " << (first + last) / 2.0 << "\n"
             << "}\n";
}

void RootEventModule::Finalize() {}
