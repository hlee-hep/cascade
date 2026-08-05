#include "ToyDimuonSourceModule.hh"

#include "Logger.hh"

#include <TFile.h>
#include <TLorentzVector.h>
#include <TRandom3.h>
#include <TTree.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>

namespace
{
constexpr double MuonMassGeV = 0.1056583755;
constexpr double Pi = 3.14159265358979323846;

double SampleTruncatedBreitWigner(TRandom3 &random, double mean, double width, double minimum, double maximum)
{
    for (int attempt = 0; attempt < 10000; ++attempt)
    {
        const double value = random.BreitWigner(mean, width);
        if (value >= minimum && value <= maximum) return value;
    }
    throw std::runtime_error("could not sample a resonance mass inside the configured range");
}

double SampleTruncatedExponential(TRandom3 &random, double minimum, double maximum, double slope)
{
    const double unit = random.Uniform();
    if (std::abs(slope) < 1e-12) return minimum + unit * (maximum - minimum);
    const double normalization = 1.0 - std::exp(-slope * (maximum - minimum));
    return minimum - std::log(1.0 - unit * normalization) / slope;
}

double SampleTruncatedGaussian(TRandom3 &random, double sigma, double maximumAbsolute)
{
    for (int attempt = 0; attempt < 10000; ++attempt)
    {
        const double value = random.Gaus(0.0, sigma);
        if (std::abs(value) <= maximumAbsolute) return value;
    }
    throw std::runtime_error("could not sample a pair rapidity inside the configured range");
}
} // namespace

ToyDimuonSourceModule::ToyDimuonSourceModule()
{
    Parameters().Register<std::string>("output", "toy_dimuons.root", "Generated ROOT event file");
    Parameters().Register<std::string>("metadata", "generation.json", "Generation metadata JSON");
    Parameters().Register<int>("events", 30000, "Number of generated dimuon candidates");
    Parameters().Register<int>("random_seed", 42, "Deterministic ROOT random seed");
    Parameters().Register<double>("resonance_mass", 91.1876, "Generated resonance pole mass in GeV");
    Parameters().Register<double>("resonance_width", 2.4952, "Generated Breit-Wigner width in GeV");
    Parameters().Register<double>("signal_fraction", 0.65, "Fraction of generated resonance events");
    Parameters().Register<double>("mass_min", 60.0, "Minimum generated dimuon mass in GeV");
    Parameters().Register<double>("mass_max", 120.0, "Maximum generated dimuon mass in GeV");
    Parameters().Register<double>("background_slope", 0.025, "Continuum exponential slope in 1/GeV");
    Parameters().Register<double>("momentum_resolution", 0.012, "Relative single-muon pT resolution");
}

void ToyDimuonSourceModule::Description() const
{
    LOG_INFO(BaseName(), "Generates a deterministic toy Z-to-dimuon sample with continuum background.");
}

ModuleMetadata ToyDimuonSourceModule::GetMetadata() const
{
    return {BaseName(), "1.0.0", "Generates a self-contained toy dimuon ROOT tree.",
            {"example", "dimuon", "generator", "root"}};
}

void ToyDimuonSourceModule::Init()
{
    const int events = Parameters().Get<int>("events");
    const double mass = Parameters().Get<double>("resonance_mass");
    const double width = Parameters().Get<double>("resonance_width");
    const double signalFraction = Parameters().Get<double>("signal_fraction");
    const double minimum = Parameters().Get<double>("mass_min");
    const double maximum = Parameters().Get<double>("mass_max");
    const double backgroundSlope = Parameters().Get<double>("background_slope");
    const double resolution = Parameters().Get<double>("momentum_resolution");

    if (events < 100) throw std::invalid_argument("events must be at least 100");
    if (!(minimum > 2.0 * MuonMassGeV && maximum > minimum))
        throw std::invalid_argument("mass range must be ordered and above the dimuon threshold");
    if (!(mass > minimum && mass < maximum)) throw std::invalid_argument("resonance_mass must be inside the mass range");
    if (!(width > 0.0 && width < maximum - minimum)) throw std::invalid_argument("resonance_width is invalid");
    if (!(signalFraction >= 0.0 && signalFraction <= 1.0)) throw std::invalid_argument("signal_fraction must be in [0, 1]");
    if (backgroundSlope < 0.0) throw std::invalid_argument("background_slope must be non-negative");
    if (!(resolution >= 0.0 && resolution < 0.5)) throw std::invalid_argument("momentum_resolution must be in [0, 0.5)");
}

void ToyDimuonSourceModule::Execute()
{
    const int requestedEvents = Parameters().Get<int>("events");
    const int randomSeed = Parameters().Get<int>("random_seed");
    const double poleMass = Parameters().Get<double>("resonance_mass");
    const double naturalWidth = Parameters().Get<double>("resonance_width");
    const double signalFraction = Parameters().Get<double>("signal_fraction");
    const double minimumMass = Parameters().Get<double>("mass_min");
    const double maximumMass = Parameters().Get<double>("mass_max");
    const double backgroundSlope = Parameters().Get<double>("background_slope");
    const double momentumResolution = Parameters().Get<double>("momentum_resolution");

    const auto stagedRoot = StageOutput(Parameters().Get<std::string>("output"));
    TFile output(stagedRoot.c_str(), "RECREATE");
    if (output.IsZombie()) throw std::runtime_error("cannot create staged ROOT output");

    TTree tree("Events", "Toy dimuon candidates");
    int event = 0;
    int mu1Charge = -1;
    int mu2Charge = 1;
    bool isSignal = false;
    double weight = 1.0;
    double generatedMass = 0.0;
    double mu1Pt = 0.0;
    double mu1Eta = 0.0;
    double mu1Phi = 0.0;
    double mu2Pt = 0.0;
    double mu2Eta = 0.0;
    double mu2Phi = 0.0;

    tree.Branch("event", &event);
    tree.Branch("is_signal", &isSignal);
    tree.Branch("weight", &weight);
    tree.Branch("generated_mass", &generatedMass);
    tree.Branch("mu1_pt", &mu1Pt);
    tree.Branch("mu1_eta", &mu1Eta);
    tree.Branch("mu1_phi", &mu1Phi);
    tree.Branch("mu1_charge", &mu1Charge);
    tree.Branch("mu2_pt", &mu2Pt);
    tree.Branch("mu2_eta", &mu2Eta);
    tree.Branch("mu2_phi", &mu2Phi);
    tree.Branch("mu2_charge", &mu2Charge);

    TRandom3 random(static_cast<unsigned int>(randomSeed));
    int generatedSignal = 0;
    for (event = 0; event < requestedEvents; ++event)
    {
        if ((event & 1023) == 0 && IsCancellationRequested()) break;

        isSignal = random.Uniform() < signalFraction;
        generatedMass = isSignal
                            ? SampleTruncatedBreitWigner(random, poleMass, naturalWidth, minimumMass, maximumMass)
                            : SampleTruncatedExponential(random, minimumMass, maximumMass, backgroundSlope);
        generatedSignal += isSignal ? 1 : 0;

        const double momentum = std::sqrt(std::max(0.0, generatedMass * generatedMass / 4.0 - MuonMassGeV * MuonMassGeV));
        const double cosine = random.Uniform(-1.0, 1.0);
        const double sine = std::sqrt(std::max(0.0, 1.0 - cosine * cosine));
        const double decayPhi = random.Uniform(-Pi, Pi);

        TLorentzVector muon1Rest(momentum * sine * std::cos(decayPhi), momentum * sine * std::sin(decayPhi),
                                 momentum * cosine, generatedMass / 2.0);
        TLorentzVector muon2Rest(-muon1Rest.Px(), -muon1Rest.Py(), -muon1Rest.Pz(), generatedMass / 2.0);

        const double pairPt = std::min(random.Exp(18.0), 120.0);
        const double pairPhi = random.Uniform(-Pi, Pi);
        const double pairRapidity = SampleTruncatedGaussian(random, 1.15, 2.2);
        const double transverseMass = std::sqrt(generatedMass * generatedMass + pairPt * pairPt);
        TLorentzVector pair(pairPt * std::cos(pairPhi), pairPt * std::sin(pairPhi),
                            transverseMass * std::sinh(pairRapidity), transverseMass * std::cosh(pairRapidity));

        muon1Rest.Boost(pair.BoostVector());
        muon2Rest.Boost(pair.BoostVector());
        const double smear1 = std::max(0.1, random.Gaus(1.0, momentumResolution));
        const double smear2 = std::max(0.1, random.Gaus(1.0, momentumResolution));
        muon1Rest.SetPtEtaPhiM(muon1Rest.Pt() * smear1, muon1Rest.Eta(), muon1Rest.Phi(), MuonMassGeV);
        muon2Rest.SetPtEtaPhiM(muon2Rest.Pt() * smear2, muon2Rest.Eta(), muon2Rest.Phi(), MuonMassGeV);

        if (random.Uniform() < 0.5)
        {
            mu1Charge = -1;
            mu2Charge = 1;
        }
        else
        {
            mu1Charge = 1;
            mu2Charge = -1;
        }
        mu1Pt = muon1Rest.Pt();
        mu1Eta = muon1Rest.Eta();
        mu1Phi = muon1Rest.Phi();
        mu2Pt = muon2Rest.Pt();
        mu2Eta = muon2Rest.Eta();
        mu2Phi = muon2Rest.Phi();
        tree.Fill();
    }

    tree.Write();
    output.Close();

    nlohmann::json metadata = {
        {"model", "truncated Breit-Wigner signal plus exponential continuum"},
        {"tree", "Events"},
        {"requested_events", requestedEvents},
        {"generated_events", static_cast<int>(tree.GetEntries())},
        {"generated_signal_events", generatedSignal},
        {"random_seed", randomSeed},
        {"resonance_mass", poleMass},
        {"resonance_width", naturalWidth},
        {"signal_fraction", signalFraction},
        {"mass_range", {minimumMass, maximumMass}},
        {"momentum_resolution", momentumResolution},
    };
    std::ofstream metadataOutput(StageOutput(Parameters().Get<std::string>("metadata")));
    if (!metadataOutput) throw std::runtime_error("cannot create staged generation metadata");
    metadataOutput << metadata.dump(2) << '\n';
}

void ToyDimuonSourceModule::Finalize() {}
