#include "ResonanceFitModule.hh"

#include "Logger.hh"

#include <TCanvas.h>
#include <TFile.h>
#include <TF1.h>
#include <TFitResult.h>
#include <TFitResultPtr.h>
#include <TH1D.h>
#include <TLegend.h>
#include <TLine.h>
#include <TMath.h>
#include <TPad.h>
#include <TStyle.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>

ResonanceFitModule::ResonanceFitModule()
{
    Parameters().Register<std::string>("input", "dimuon_spectrum.root", "Input spectrum ROOT file");
    Parameters().Register<std::string>("histogram", "dimuon_mass", "Input histogram name");
    Parameters().Register<std::string>("output_root", "dimuon_fit.root", "ROOT fit products");
    Parameters().Register<std::string>("output_json", "fit_result.json", "Machine-readable fit result");
    Parameters().Register<std::string>("output_plot", "dimuon_fit.png", "Mass fit and pull plot");
    Parameters().Register<double>("fit_min", 70.0, "Minimum fitted mass in GeV");
    Parameters().Register<double>("fit_max", 110.0, "Maximum fitted mass in GeV");
    Parameters().Register<double>("mass_seed", 91.1876, "Initial resonance mass in GeV");
    Parameters().Register<double>("natural_width", 2.4952, "Initial or fixed natural width in GeV");
    Parameters().Register<double>("resolution_seed", 1.2, "Initial Gaussian detector resolution in GeV");
    Parameters().Register<bool>("float_natural_width", false, "Allow the natural width to float in the fit");
}

void ResonanceFitModule::Description() const
{
    LOG_INFO(BaseName(), "Fits the dimuon spectrum with a Voigt signal and exponential background.");
}

ModuleMetadata ResonanceFitModule::GetMetadata() const
{
    return {BaseName(), "1.0.0", "Performs a binned-likelihood resonance mass fit and produces a pull plot.",
            {"example", "dimuon", "fit", "voigt", "root"}};
}

void ResonanceFitModule::Init()
{
    const auto input = FinalOutput(Parameters().Get<std::string>("input"));
    if (!std::filesystem::is_regular_file(input)) throw std::runtime_error("input spectrum does not exist: " + input.string());
    TrackInput(input);

    const double fitMinimum = Parameters().Get<double>("fit_min");
    const double fitMaximum = Parameters().Get<double>("fit_max");
    const double massSeed = Parameters().Get<double>("mass_seed");
    const double naturalWidth = Parameters().Get<double>("natural_width");
    const double resolutionSeed = Parameters().Get<double>("resolution_seed");
    if (!(fitMinimum > 0.0 && fitMaximum > fitMinimum)) throw std::invalid_argument("fit range is invalid");
    if (!(massSeed > fitMinimum && massSeed < fitMaximum)) throw std::invalid_argument("mass_seed must be inside the fit range");
    if (!(naturalWidth > 0.0 && naturalWidth < fitMaximum - fitMinimum)) throw std::invalid_argument("natural_width is invalid");
    if (!(resolutionSeed > 0.0 && resolutionSeed < fitMaximum - fitMinimum))
        throw std::invalid_argument("resolution_seed is invalid");
}

void ResonanceFitModule::Execute()
{
    const auto inputPath = FinalOutput(Parameters().Get<std::string>("input"));
    const std::string histogramName = Parameters().Get<std::string>("histogram");
    const double fitMinimum = Parameters().Get<double>("fit_min");
    const double fitMaximum = Parameters().Get<double>("fit_max");
    const double massSeed = Parameters().Get<double>("mass_seed");
    const double naturalWidth = Parameters().Get<double>("natural_width");
    const double resolutionSeed = Parameters().Get<double>("resolution_seed");
    const bool floatNaturalWidth = Parameters().Get<bool>("float_natural_width");

    TFile input(inputPath.c_str(), "READ");
    if (input.IsZombie()) throw std::runtime_error("cannot open input spectrum ROOT file");
    auto *sourceHistogram = dynamic_cast<TH1D *>(input.Get(histogramName.c_str()));
    if (!sourceHistogram) throw std::runtime_error("input histogram is missing or is not TH1D: " + histogramName);
    std::unique_ptr<TH1D> histogram(static_cast<TH1D *>(sourceHistogram->Clone("dimuon_mass_data")));
    histogram->SetDirectory(nullptr);
    input.Close();

    const double binWidth = histogram->GetXaxis()->GetBinWidth(1);
    const int firstFitBin = histogram->GetXaxis()->FindFixBin(fitMinimum + 1e-9);
    const int lastFitBin = histogram->GetXaxis()->FindFixBin(fitMaximum - 1e-9);
    const double observedYield = histogram->Integral(firstFitBin, lastFitBin);
    if (observedYield < 100.0) throw std::runtime_error("fit range contains too few events");

    TF1 model(
        "dimuon_fit_model",
        [binWidth, massSeed](double *coordinate, double *parameter)
        {
            const double signal = parameter[0] * binWidth *
                                  TMath::Voigt(coordinate[0] - parameter[1], parameter[2], parameter[3], 4);
            const double background = std::exp(parameter[4] + parameter[5] * (coordinate[0] - massSeed));
            return signal + background;
        },
        fitMinimum, fitMaximum, 6);
    model.SetParNames("signal_yield", "mass", "resolution", "natural_width", "background_log_norm",
                      "background_slope");
    const double averageBinContent = observedYield / std::max(1, lastFitBin - firstFitBin + 1);
    model.SetParameters(observedYield * 0.65, massSeed, resolutionSeed, naturalWidth,
                        std::log(std::max(1.0, averageBinContent * 0.35)), -0.02);
    model.SetParLimits(0, 0.0, observedYield * 2.0);
    model.SetParLimits(1, std::max(fitMinimum, massSeed - 5.0), std::min(fitMaximum, massSeed + 5.0));
    model.SetParLimits(2, 0.05, 8.0);
    model.SetParLimits(3, 0.1, 12.0);
    model.SetParLimits(4, -20.0, 20.0);
    model.SetParLimits(5, -1.0, 1.0);
    if (!floatNaturalWidth) model.FixParameter(3, naturalWidth);

    TFitResultPtr fitResult = histogram->Fit(&model, "SLRQ0", "", fitMinimum, fitMaximum);
    const int fitStatus = fitResult;
    if (!fitResult.Get()) throw std::runtime_error("ROOT did not return a fit result");
    if (fitStatus != 0 || !fitResult->IsValid())
        throw std::runtime_error("resonance fit failed with status " + std::to_string(fitStatus));

    TF1 signal(
        "dimuon_signal",
        [binWidth](double *coordinate, double *parameter)
        { return parameter[0] * binWidth * TMath::Voigt(coordinate[0] - parameter[1], parameter[2], parameter[3], 4); },
        fitMinimum, fitMaximum, 4);
    for (int index = 0; index < 4; ++index) signal.SetParameter(index, model.GetParameter(index));
    TF1 background(
        "dimuon_background",
        [massSeed](double *coordinate, double *parameter)
        { return std::exp(parameter[0] + parameter[1] * (coordinate[0] - massSeed)); },
        fitMinimum, fitMaximum, 2);
    background.SetParameters(model.GetParameter(4), model.GetParameter(5));

    std::unique_ptr<TH1D> pull(static_cast<TH1D *>(histogram->Clone("dimuon_mass_pull")));
    pull->Reset("ICES");
    pull->SetDirectory(nullptr);
    pull->SetTitle(";m_{#mu#mu} [GeV];Pull");
    for (int bin = firstFitBin; bin <= lastFitBin; ++bin)
    {
        const double observed = histogram->GetBinContent(bin);
        const double expected = model.Eval(histogram->GetBinCenter(bin));
        const double uncertainty = histogram->GetBinError(bin) > 0.0 ? histogram->GetBinError(bin)
                                                                       : std::sqrt(std::max(1.0, observed));
        pull->SetBinContent(bin, (observed - expected) / uncertainty);
    }

    gStyle->SetOptStat(0);
    TCanvas canvas("dimuon_fit_canvas", "Toy dimuon resonance fit", 1000, 850);
    TPad upper("upper", "Mass fit", 0.0, 0.30, 1.0, 1.0);
    TPad lower("lower", "Pull", 0.0, 0.0, 1.0, 0.30);
    upper.SetBottomMargin(0.03);
    lower.SetTopMargin(0.04);
    lower.SetBottomMargin(0.32);
    upper.Draw();
    lower.Draw();

    upper.cd();
    histogram->SetTitle("Toy Z #rightarrow #mu^{+}#mu^{-};m_{#mu#mu} [GeV];Events / bin");
    histogram->GetXaxis()->SetRangeUser(fitMinimum, fitMaximum);
    histogram->GetXaxis()->SetLabelSize(0.0);
    histogram->SetMarkerStyle(20);
    histogram->SetMarkerSize(0.65);
    histogram->SetLineColor(kBlack);
    histogram->Draw("E1");
    model.SetLineColor(kRed + 1);
    model.SetLineWidth(3);
    model.Draw("SAME");
    signal.SetLineColor(kBlue + 1);
    signal.SetLineStyle(2);
    signal.SetLineWidth(2);
    signal.Draw("SAME");
    background.SetLineColor(kGreen + 2);
    background.SetLineStyle(3);
    background.SetLineWidth(2);
    background.Draw("SAME");
    TLegend legend(0.59, 0.64, 0.88, 0.88);
    legend.SetBorderSize(0);
    legend.SetFillStyle(0);
    legend.AddEntry(histogram.get(), "Toy data", "lep");
    legend.AddEntry(&model, "Voigt + exponential", "l");
    legend.AddEntry(&signal, "Signal", "l");
    legend.AddEntry(&background, "Background", "l");
    legend.Draw();

    lower.cd();
    pull->GetXaxis()->SetRangeUser(fitMinimum, fitMaximum);
    pull->GetXaxis()->SetTitleSize(0.12);
    pull->GetXaxis()->SetLabelSize(0.10);
    pull->GetYaxis()->SetTitleSize(0.10);
    pull->GetYaxis()->SetLabelSize(0.09);
    pull->GetYaxis()->SetTitleOffset(0.45);
    pull->GetYaxis()->SetNdivisions(505);
    pull->SetMinimum(-5.0);
    pull->SetMaximum(5.0);
    pull->SetMarkerStyle(20);
    pull->SetMarkerSize(0.55);
    pull->Draw("P");
    TLine zero(fitMinimum, 0.0, fitMaximum, 0.0);
    zero.SetLineStyle(2);
    zero.Draw();
    canvas.cd();
    canvas.SaveAs(StageOutput(Parameters().Get<std::string>("output_plot")).c_str());

    TFile rootOutput(StageOutput(Parameters().Get<std::string>("output_root")).c_str(), "RECREATE");
    if (rootOutput.IsZombie()) throw std::runtime_error("cannot create staged fit ROOT file");
    histogram->Write();
    model.Write();
    signal.Write();
    background.Write();
    pull->Write();
    canvas.Write();
    rootOutput.Close();

    const int ndf = model.GetNDF();
    nlohmann::json result = {
        {"model", "Voigt signal plus exponential background"},
        {"fit_status", fitStatus},
        {"fit_valid", fitResult->IsValid()},
        {"covariance_quality", fitResult->CovMatrixStatus()},
        {"fit_range", {fitMinimum, fitMaximum}},
        {"observed_yield", observedYield},
        {"chi2", model.GetChisquare()},
        {"ndf", ndf},
        {"chi2_ndf", ndf > 0 ? model.GetChisquare() / static_cast<double>(ndf) : 0.0},
        {"parameters",
         {{"mass", {{"value", model.GetParameter(1)}, {"error", model.GetParError(1)}, {"unit", "GeV"}}},
          {"resolution", {{"value", model.GetParameter(2)}, {"error", model.GetParError(2)}, {"unit", "GeV"}}},
          {"natural_width",
           {{"value", model.GetParameter(3)},
            {"error", floatNaturalWidth ? model.GetParError(3) : 0.0},
            {"unit", "GeV"},
            {"fixed", !floatNaturalWidth}}},
          {"signal_yield", {{"value", model.GetParameter(0)}, {"error", model.GetParError(0)}}},
          {"signal_yield_in_range", signal.Integral(fitMinimum, fitMaximum) / binWidth},
          {"background_yield_in_range", background.Integral(fitMinimum, fitMaximum) / binWidth},
          {"background_slope", {{"value", model.GetParameter(5)}, {"error", model.GetParError(5)}, {"unit", "1/GeV"}}}}},
    };
    std::ofstream jsonOutput(StageOutput(Parameters().Get<std::string>("output_json")));
    if (!jsonOutput) throw std::runtime_error("cannot create staged fit result JSON");
    jsonOutput << result.dump(2) << '\n';
}

void ResonanceFitModule::Finalize() {}
