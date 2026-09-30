#include "PlotManager.hh"

#include <TColor.h>
#include <TROOT.h>
#include <nlohmann/json.hpp>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <spawn.h>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace
{
using Json = nlohmann::json;

struct PlanOwner
{
    RenderPlan &Plan;
    ~PlanOwner()
    {
        delete Plan.StackBand;
        delete Plan.StackSum;
        delete Plan.Frame;
        for (auto *object : Plan.OwnedInputs) delete object;
    }
};

std::string HexColor(int index)
{
    auto *color = gROOT->GetColor(index);
    return color ? color->AsHexString() : "#000000";
}

Json Color(const ColorSpec &color)
{
    return {{"line", HexColor(color.Line)}, {"fill", HexColor(color.Fill)},
            {"marker", HexColor(color.Marker)}, {"marker_style", color.MarkerStyle}};
}

Json Histogram(const TH1 &hist)
{
    Json result = {{"kind", "hist"}, {"edges", Json::array()}, {"values", Json::array()},
                   {"error_low", Json::array()}, {"error_high", Json::array()}};
    for (int bin = 1; bin <= hist.GetNbinsX(); ++bin)
    {
        result["edges"].push_back(hist.GetXaxis()->GetBinLowEdge(bin));
        result["values"].push_back(hist.GetBinContent(bin));
        result["error_low"].push_back(hist.GetBinErrorLow(bin));
        result["error_high"].push_back(hist.GetBinErrorUp(bin));
    }
    result["edges"].push_back(hist.GetXaxis()->GetBinUpEdge(hist.GetNbinsX()));
    return result;
}

Json Histogram2D(const TH2 &hist)
{
    // Row-major [y][x], matching Matplotlib pcolormesh without a transpose.
    Json result = {{"kind", "hist2d"}, {"xedges", Json::array()}, {"yedges", Json::array()},
                   {"values", Json::array()}, {"errors", Json::array()}, {"zlabel", hist.GetZaxis()->GetTitle()}};
    for (int ix = 1; ix <= hist.GetNbinsX() + 1; ++ix)
        result["xedges"].push_back(hist.GetXaxis()->GetBinLowEdge(ix));
    for (int iy = 1; iy <= hist.GetNbinsY() + 1; ++iy)
        result["yedges"].push_back(hist.GetYaxis()->GetBinLowEdge(iy));
    for (int iy = 1; iy <= hist.GetNbinsY(); ++iy)
    {
        Json values = Json::array(), errors = Json::array();
        for (int ix = 1; ix <= hist.GetNbinsX(); ++ix)
        {
            values.push_back(hist.GetBinContent(ix, iy));
            errors.push_back(hist.GetBinError(ix, iy));
        }
        result["values"].push_back(std::move(values));
        result["errors"].push_back(std::move(errors));
    }
    result["ylim"] = {hist.GetYaxis()->GetBinLowEdge(hist.GetYaxis()->GetFirst()),
                       hist.GetYaxis()->GetBinUpEdge(hist.GetYaxis()->GetLast())};
    result["zmin"] = hist.GetMinimumStored() == -1111 ? Json(nullptr) : Json(hist.GetMinimumStored());
    result["zmax"] = hist.GetMaximumStored() == -1111 ? Json(nullptr) : Json(hist.GetMaximumStored());
    return result;
}

Json Graph(const TGraph &graph)
{
    Json result = {{"kind", "graph"}, {"x", Json::array()}, {"y", Json::array()},
                   {"xerror_low", Json::array()}, {"xerror_high", Json::array()},
                   {"error_low", Json::array()}, {"error_high", Json::array()}};
    for (int i = 0; i < graph.GetN(); ++i)
    {
        result["x"].push_back(graph.GetPointX(i));
        result["y"].push_back(graph.GetPointY(i));
        result["xerror_low"].push_back(std::max(0., graph.GetErrorXlow(i)));
        result["xerror_high"].push_back(std::max(0., graph.GetErrorXhigh(i)));
        result["error_low"].push_back(std::max(0., graph.GetErrorYlow(i)));
        result["error_high"].push_back(std::max(0., graph.GetErrorYhigh(i)));
    }
    return result;
}

void Describe(Json &item, const std::string &label, const ColorSpec &color, const DrawSpec &draw,
              const LegendSpec &legend, bool empty)
{
    item["label"] = label;
    item["color"] = Color(color);
    item["draw"] = draw.DrawOpt;
    item["legend"] = legend.Enable && draw.VisibleInLegend && !(legend.SkipEmpty && empty);
    item["priority"] = draw.LegendPriority.value_or(0);
    item["zero_error"] = draw.ZeroError;
}

struct TemporaryRequest
{
    std::string Path;
    ~TemporaryRequest() { if (!Path.empty()) ::unlink(Path.c_str()); }
};
void RunPublication(const Json &document, const std::string &output, const std::string &layout, const std::string &python)
{
    if (output.empty() || python.empty()) throw std::invalid_argument("Publication output and Python executable must not be empty");
    std::string pattern = (std::filesystem::temp_directory_path() / "cascade-publication-XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    const int fd = ::mkstemp(buffer.data());
    if (fd < 0) throw std::runtime_error("Cannot create publication request: " + std::string(std::strerror(errno)));
    TemporaryRequest request{buffer.data()};
    ::close(fd);
    {
        std::ofstream stream(request.Path);
        stream.exceptions(std::ios::badbit | std::ios::failbit);
        stream << document.dump();
    }
    // Argument vector, not a shell: paths/labels cannot be interpreted as commands.
    std::vector<std::string> arguments = {python, "-m", "cascade.publication", "--input", request.Path,
                                         "--output", output, "--layout", layout};
    std::vector<char *> argv;
    for (auto &argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    pid_t pid;
    const int error = ::posix_spawnp(&pid, python.c_str(), nullptr, nullptr, argv.data(), environ);
    if (error) throw std::runtime_error("Cannot start publication renderer '" + python + "': " + std::strerror(error));
    int status = 0;
    pid_t waited;
    do { waited = ::waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        throw std::runtime_error("Publication renderer failed for '" + output + "'; see Python diagnostics above");
}

} // namespace

std::string PlotManager::ExportPublication(const PlotSpec &spec) const
{
    ValidateSpec_(spec);
    int histograms = 0, heatmaps = 0;
    bool visibleStack = false;
    for (const auto &item : spec.Stacks) visibleStack = visibleStack || item.Draw.Visible;
    for (const auto &item : spec.Overlays)
    {
        if (!item.Draw.Visible || item.Kind != ItemKind::Hist) continue;
        ++histograms;
        if (item.H->GetDimension() == 2) ++heatmaps;
        else if (item.H->GetDimension() != 1)
            throw std::invalid_argument("Publication histograms must be 1D or 2D");
    }
    if (heatmaps && (heatmaps != 1 || histograms != 1 || visibleStack || spec.Ratio.Enable))
        throw std::invalid_argument("A publication 2D panel accepts one TH2 plus graph overlays, without stacks or ratio");

    RenderPlan plan;
    PlanOwner owner{plan};
    BuildPlan_(spec, plan);
    Json result = {{"schema_version", 1}, {"xlabel", spec.XTitle}, {"ylabel", spec.YTitle},
                   {"log_y", spec.Theme.LogY}, {"stacks", Json::array()}, {"overlays", Json::array()},
                   {"legend_columns", std::max(1, spec.Legend.NCol)}, {"legend_enable", spec.Legend.Enable},
                   {"sample", {{"enabled", spec.Sample.Enable}, {"experiment", spec.Sample.Experiment},
                               {"comment", spec.Sample.Comment}, {"luminosity", spec.Sample.Lumi},
                               {"luminosity_unit", spec.Sample.LumiUnit}}},
                   {"ymin", spec.Layout.ForceYMin ? Json(spec.Layout.YMin) : Json(nullptr)},
                   {"ymax", spec.Layout.ForceYMax ? Json(spec.Layout.YMax) : Json(nullptr)},
                   {"cut_lower", spec.Cut.DnCut ? Json(*spec.Cut.DnCut) : Json(nullptr)},
                   {"cut_upper", spec.Cut.UpCut ? Json(*spec.Cut.UpCut) : Json(nullptr)}};
    bool manualOrder = spec.Legend.Mode == LegendMode::Manual;
    for (const auto &item : plan.Stacks)
        if (item.Draw.VisibleInLegend && !(spec.Legend.SkipEmpty && IsEmptyObject_(item)) && !item.Draw.LegendPriority)
            manualOrder = false;
    for (const auto &item : plan.Overlays)
        if (item.Draw.VisibleInLegend && !(spec.Legend.SkipEmpty && IsEmptyObject_(item)) && !item.Draw.LegendPriority)
            manualOrder = false;
    int stackOrder = static_cast<int>(plan.Overlays.size());
    int overlayOrder = 0;
    bool hasHistogram = !plan.Stacks.empty();
    for (const auto &item : plan.Stacks)
    {
        Json out = Histogram(*item.H);
        Describe(out, item.Label, item.Color, item.Draw, spec.Legend, IsEmptyObject_(item));
        out["priority"] = manualOrder ? item.Draw.LegendPriority.value_or(0) : stackOrder++;
        result["stacks"].push_back(std::move(out));
    }
    for (auto &item : plan.Overlays)
    {
        Json out;
        if (item.Kind == ItemKind::Hist)
        {
            hasHistogram = true;
            // Match Draw(): view transforms precede data errors / stack normalization.
            if (item.IsData) item.H->SetBinErrorOption(TH1::kPoisson);
            else if (item.Draw.Scale && *item.Draw.Scale < 0 && plan.StackSum && item.H->Integral() != 0.)
                item.H->Scale(plan.StackSum->Integral() / item.H->Integral());
            if (item.H->GetDimension() == 2)
            {
                out = Histogram2D(*static_cast<TH2 *>(item.H));
                out["log_z"] = spec.Theme.LogZ;
            }
            else out = Histogram(*item.H);
        }
        else out = Graph(*(item.Kind == ItemKind::Graph ? item.G : item.GAE));
        Describe(out, item.Label, item.Color, item.Draw, spec.Legend, IsEmptyObject_(item));
        out["priority"] = manualOrder ? item.Draw.LegendPriority.value_or(0) : overlayOrder++;
        out["is_data"] = item.IsData;
        result["overlays"].push_back(std::move(out));
    }
    if (hasHistogram)
        result["xlim"] = {plan.Frame->GetXaxis()->GetBinLowEdge(plan.Frame->GetXaxis()->GetFirst()),
                          plan.Frame->GetXaxis()->GetBinUpEdge(plan.Frame->GetXaxis()->GetLast())};
    if (plan.StackBand)
    {
        result["band"] = Graph(*plan.StackBand);
        result["band"]["label"] = spec.Band.Name;
        result["band"]["edges"] = Histogram(*plan.StackSum)["edges"];
    }
    if (spec.Ratio.Enable)
    {
        const auto pair = FindRatioPair_(spec, plan);
        if (!pair.first || !pair.second) throw std::invalid_argument("Publication ratio requires numerator and denominator");
        for (int bin = 1; bin <= pair.first->GetNbinsX(); ++bin)
            if (spec.Band.Asymm && (pair.first->GetBinContent(bin) < 0 || pair.second->GetBinContent(bin) < 0))
                throw std::invalid_argument("Poisson ratio intervals require nonnegative bin contents; use Band.Asymm=false for signed weights");
        auto calculated = MakeRatio_(pair.first, pair.second, "pm_publication_ratio");
        std::unique_ptr<TH1> symmetric(calculated.first);
        std::unique_ptr<TGraphAsymmErrors> asymmetric(calculated.second);
        if (!symmetric || !asymmetric) throw std::runtime_error("Cannot prepare publication ratio");
        TGraphAsymmErrors symmetricPoints;
        if (!spec.Band.Asymm)
        {
            for (int bin = 1; bin <= symmetric->GetNbinsX(); ++bin)
            {
                if (pair.second->GetBinContent(bin) == 0) continue;
                const int index = symmetricPoints.GetN();
                symmetricPoints.SetPoint(index, symmetric->GetBinCenter(bin), symmetric->GetBinContent(bin));
                const double error = symmetric->GetBinError(bin);
                symmetricPoints.SetPointError(index, 0, 0, error, error);
            }
        }
        result["ratio"] = {{"points", Graph(spec.Band.Asymm ? *asymmetric : symmetricPoints)},
            {"ylabel", spec.Ratio.YLabel}, {"ymin", spec.Ratio.YMin}, {"ymax", spec.Ratio.YMax},
            {"split", spec.Layout.RatioSplit}, {"unity_line", spec.Ratio.UnityLine}, {"arrows", spec.Ratio.Arrow},
            {"error_mode", spec.Band.Asymm ? "poisson" : "symmetric"}};
        if (spec.Ratio.MCError)
        {
            std::unique_ptr<TGraphAsymmErrors> errors(MakeBandFromHist_(pair.second));
            TGraphAsymmErrors relative;
            for (int bin = 1; bin <= pair.second->GetNbinsX(); ++bin)
            {
                const double value = pair.second->GetBinContent(bin);
                if (value == 0) continue; // No defined ratio or band for a zero denominator.
                const int index = relative.GetN();
                relative.SetPoint(index, pair.second->GetBinCenter(bin), 1.);
                const double width = pair.second->GetBinWidth(bin)/2.;
                relative.SetPointError(index, width, width, errors->GetErrorYlow(bin-1)/std::abs(value),
                                       errors->GetErrorYhigh(bin-1)/std::abs(value));
            }
            result["ratio"]["band"] = Graph(relative);
        }
    }
    return result.dump();
}

void PlotManager::SavePublication(const std::vector<PlotSpec> &panels, const std::string &output,
                                  const std::string &layout, const std::string &python) const
{
    const std::size_t count = layout == "single" ? 1 : layout == "vertical2" ? 2 : layout == "row4" ? 4 : 0;
    if (count == 0 || panels.size() != count)
        throw std::invalid_argument("Publication layout must be single (1 panel), vertical2 (2), or row4 (4)");
    Json document = {{"schema_version", 1}, {"panels", Json::array()}};
    for (const auto &panel : panels) document["panels"].push_back(Json::parse(ExportPublication(panel)));
    RunPublication(document, output, layout, python);
}

void PlotManager::SavePublication(const std::vector<PlotSpec> &panels, const std::string &output,
                                  const PublicationOptions &options, const std::string &python) const
{
    const auto &l = options.Layout;
    const auto &s = options.Style;
    if (l.Rows <= 0 || l.Columns <= 0 || panels.empty() ||
        panels.size() > static_cast<std::size_t>(l.Rows)*static_cast<std::size_t>(l.Columns))
        throw std::invalid_argument("Publication panels must fit a positive row/column grid");
    Json layout = {{"rows", l.Rows}, {"columns", l.Columns}, {"width", l.Width}, {"height", l.Height},
        {"left", l.Left}, {"right", l.Right}, {"bottom", l.Bottom}, {"top", l.Top},
        {"hgap", l.HGap}, {"vgap", l.VGap}, {"legend", l.Legend}, {"legend_location", l.LegendLocation},
        {"legend_columns", l.LegendColumns}, {"share_x", l.ShareX}, {"share_y", l.ShareY},
        {"panel_labels", l.PanelLabels}, {"ratio_gap", l.RatioGap}, {"colorbar_space", l.ColorbarSpace}};
    Json style = {{"font_size", s.FontSize}, {"axis_size", s.AxisSize}, {"legend_size", s.LegendSize},
        {"marker_size", s.MarkerSize}, {"line_width", s.LineWidth}, {"header_gap", s.HeaderGap}, {"use_tex", s.UseTex}, {"color_map", s.ColorMap}, {"dpi", s.Dpi}};
    Json document = {{"schema_version", 1}, {"options", {{"layout", layout}, {"style", style}}}, {"panels", Json::array()}};
    for (const auto &panel : panels) document["panels"].push_back(Json::parse(ExportPublication(panel)));
    RunPublication(document, output, "single", python);
}
