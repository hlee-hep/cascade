#include "DimuonSpectrumModule.hh"

#include "Logger.hh"

#include <ROOT/RDataFrame.hxx>
#include <TFile.h>
#include <TH1D.h>
#include <TLorentzVector.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace
{
constexpr double MuonMassGeV = 0.1056583755;
}

DimuonSpectrumModule::DimuonSpectrumModule()
{
    Parameters().Register<std::string>("input", "toy_dimuons.root", "Input ROOT event file");
    Parameters().Register<std::string>("tree", "Events", "Input TTree name");
    Parameters().Register<std::string>("output", "dimuon_spectrum.root", "Selected mass histogram ROOT file");
    Parameters().Register<std::string>("cutflow", "cutflow.json", "Selection cutflow JSON");
    Parameters().Register<double>("pt_min", 20.0, "Minimum pT for both muons in GeV");
    Parameters().Register<double>("eta_max", 2.4, "Maximum absolute pseudorapidity for both muons");
    Parameters().Register<double>("mass_min", 60.0, "Minimum selected dimuon mass in GeV");
    Parameters().Register<double>("mass_max", 120.0, "Maximum selected dimuon mass in GeV");
    Parameters().Register<int>("mass_bins", 120, "Number of dimuon mass histogram bins");
}

void DimuonSpectrumModule::Description() const
{
    LOG_INFO(BaseName(), "Builds a selected dimuon mass spectrum with ROOT RDataFrame.");
}

ModuleMetadata DimuonSpectrumModule::GetMetadata() const
{
    return {BaseName(), "1.0.0", "Selects toy dimuon events and writes the mass spectrum.",
            {"example", "dimuon", "rdf", "selection", "root"}};
}

void DimuonSpectrumModule::Init()
{
    const auto input = FinalOutput(Parameters().Get<std::string>("input"));
    if (!std::filesystem::is_regular_file(input)) throw std::runtime_error("input ROOT file does not exist: " + input.string());
    TrackInput(input);

    const double ptMinimum = Parameters().Get<double>("pt_min");
    const double etaMaximum = Parameters().Get<double>("eta_max");
    const double massMinimum = Parameters().Get<double>("mass_min");
    const double massMaximum = Parameters().Get<double>("mass_max");
    const int massBins = Parameters().Get<int>("mass_bins");
    if (ptMinimum < 0.0) throw std::invalid_argument("pt_min must be non-negative");
    if (!(etaMaximum > 0.0 && etaMaximum <= 10.0)) throw std::invalid_argument("eta_max is invalid");
    if (!(massMinimum > 0.0 && massMaximum > massMinimum)) throw std::invalid_argument("mass range is invalid");
    if (massBins < 10 || massBins > 10000) throw std::invalid_argument("mass_bins must be between 10 and 10000");
}

void DimuonSpectrumModule::Execute()
{
    const auto input = FinalOutput(Parameters().Get<std::string>("input"));
    const std::string treeName = Parameters().Get<std::string>("tree");
    const double ptMinimum = Parameters().Get<double>("pt_min");
    const double etaMaximum = Parameters().Get<double>("eta_max");
    const double massMinimum = Parameters().Get<double>("mass_min");
    const double massMaximum = Parameters().Get<double>("mass_max");
    const int massBins = Parameters().Get<int>("mass_bins");

    ROOT::RDataFrame dataframe(treeName, input.string());
    auto withMass = dataframe.Define(
        "dimuon_mass",
        [](double pt1, double eta1, double phi1, double pt2, double eta2, double phi2)
        {
            TLorentzVector muon1;
            TLorentzVector muon2;
            muon1.SetPtEtaPhiM(pt1, eta1, phi1, MuonMassGeV);
            muon2.SetPtEtaPhiM(pt2, eta2, phi2, MuonMassGeV);
            return (muon1 + muon2).M();
        },
        {"mu1_pt", "mu1_eta", "mu1_phi", "mu2_pt", "mu2_eta", "mu2_phi"});
    auto oppositeSign = withMass.Filter([](int charge1, int charge2) { return charge1 * charge2 < 0; },
                                        {"mu1_charge", "mu2_charge"}, "opposite-sign");
    auto accepted = oppositeSign.Filter(
        [ptMinimum, etaMaximum](double pt1, double eta1, double pt2, double eta2)
        { return pt1 >= ptMinimum && pt2 >= ptMinimum && std::abs(eta1) <= etaMaximum && std::abs(eta2) <= etaMaximum; },
        {"mu1_pt", "mu1_eta", "mu2_pt", "mu2_eta"}, "kinematic acceptance");
    auto selected = accepted.Filter([massMinimum, massMaximum](double mass)
                                    { return mass >= massMinimum && mass <= massMaximum; },
                                    {"dimuon_mass"}, "mass range");

    auto totalCount = dataframe.Count();
    auto oppositeSignCount = oppositeSign.Count();
    auto acceptedCount = accepted.Count();
    auto selectedCount = selected.Count();
    ROOT::RDF::TH1DModel model("dimuon_mass", ";m_{#mu#mu} [GeV];Events / bin", massBins, massMinimum, massMaximum);
    auto massHistogram = selected.Histo1D(model, "dimuon_mass", "weight");

    const auto total = static_cast<unsigned long long>(totalCount.GetValue());
    const auto opposite = static_cast<unsigned long long>(oppositeSignCount.GetValue());
    const auto kinematic = static_cast<unsigned long long>(acceptedCount.GetValue());
    const auto finalSelection = static_cast<unsigned long long>(selectedCount.GetValue());

    TH1D histogram(massHistogram.GetValue());
    histogram.SetDirectory(nullptr);
    histogram.SetName("dimuon_mass");
    histogram.SetTitle("Selected toy dimuon mass;m_{#mu#mu} [GeV];Events / bin");
    histogram.SetLineWidth(2);
    histogram.SetMarkerStyle(20);
    histogram.SetMarkerSize(0.7);

    TFile output(StageOutput(Parameters().Get<std::string>("output")).c_str(), "RECREATE");
    if (output.IsZombie()) throw std::runtime_error("cannot create staged spectrum ROOT file");
    histogram.Write();
    output.Close();

    nlohmann::json cutflow = {
        {"input", input.string()},
        {"tree", treeName},
        {"selection", {{"pt_min", ptMinimum}, {"eta_max", etaMaximum}, {"mass_range", {massMinimum, massMaximum}}}},
        {"counts", {{"all", total}, {"opposite_sign", opposite}, {"kinematic", kinematic}, {"mass_window", finalSelection}}},
        {"efficiency", total > 0 ? static_cast<double>(finalSelection) / static_cast<double>(total) : 0.0},
    };
    std::ofstream cutflowOutput(StageOutput(Parameters().Get<std::string>("cutflow")));
    if (!cutflowOutput) throw std::runtime_error("cannot create staged cutflow JSON");
    cutflowOutput << cutflow.dump(2) << '\n';
}

void DimuonSpectrumModule::Finalize() {}
