#pragma once

#include <TCanvas.h>
#include <TGraph.h>
#include <TGraphAsymmErrors.h>
#include <TH1.h>
#include <TH2.h>
#include <THStack.h>
#include <TLegend.h>
#include <TLine.h>
#include <TPad.h>
#include <TStyle.h>

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <vector>

enum class ItemKind
{
    Hist,
    Graph,
    GraphAsymm
};
enum class RatioRole
{
    None,
    Numerator,
    Denominator
};

struct ColorSpec
{
    int Line = 1;
    int Fill = 0;
    int Marker = 1;
    int MarkerStyle = 20;
    int LineWidth = 2;
    int FillStyle = 1001;
};

struct DrawSpec
{
    std::string DrawOpt = "";
    std::optional<int> Rebin;
    std::optional<double> Scale;
    bool NormBinWidth = false;
    bool Visible = true;
    std::string LegendOption;
};

struct StackItemSpec
{
    TH1 *H = nullptr; //
    std::string Label;
    ColorSpec Color;
    DrawSpec Draw;
};

struct OverlaySpec
{
    ItemKind Kind = ItemKind::Hist;
    TH1 *H = nullptr;                 //
    TGraph *G = nullptr;              //
    TGraphAsymmErrors *GAE = nullptr; //
    std::string Label;
    ColorSpec Color;
    DrawSpec Draw;
    bool IsData = false;
    RatioRole Role = RatioRole::None;

    static OverlaySpec Hist(TH1 *h, std::string lab, ColorSpec c, DrawSpec d = {}, bool isData = false)
    {
        OverlaySpec s;
        s.Kind = ItemKind::Hist;
        s.H = h;
        s.Label = std::move(lab);
        s.Color = c;
        s.Draw = d;
        s.IsData = isData;
        return s;
    }
    static OverlaySpec Graph(TGraph *g, std::string lab, ColorSpec c, DrawSpec d = {})
    {
        OverlaySpec s;
        s.Kind = ItemKind::Graph;
        s.G = g;
        s.Label = std::move(lab);
        s.Color = c;
        s.Draw = d;
        return s;
    }
    static OverlaySpec GraphAsymm(TGraphAsymmErrors *gae, std::string lab, ColorSpec c, DrawSpec d = {})
    {
        OverlaySpec s;
        s.Kind = ItemKind::GraphAsymm;
        s.GAE = gae;
        s.Label = std::move(lab);
        s.Color = c;
        s.Draw = d;
        return s;
    }
};

struct LegendSpec
{
    bool Enable = true;
    double X1 = 0.60, Y1 = 0.65, X2 = 0.88, Y2 = 0.88;
    int NCol = 1;
};

struct RatioSpec
{
    bool Enable = false;
    std::string YLabel = "Data/MC";
    double YMin = 0.0, YMax = 2.0;
    bool UnityLine = true;
    bool MCError = true;
    std::optional<std::string> DenominatorOverlayLabel; //
};

struct ThemeSpec
{
    int Font = 42;
    float LabelSize = 0.045f;
    float TitleSize = 0.050f;
    bool LogY = false;
};

struct BandSpec
{
    bool Enable = true;
    bool Asymm = true;
    int FillStyle = 3254;
    int FillColor = 1;
    float Alpha = 1;
};

struct LayoutSpec
{
    int CanvW = 800, CanvH = 700;
    double RatioSplit = 0.30;
    double TopPadBottomMargin = 0.04;
    double BotPadTopMargin = 0.02;
    double BotPadBottomMargin = 0.30;
    bool ForceYMin = false;
    double YMin = 0.1;
    bool ForceYMax = false;
    double YMax = 0.0;
};

struct PlotSpec
{
    std::string XTitle, YTitle = "Events", Title;
    ThemeSpec Theme;
    LayoutSpec Layout;
    LegendSpec Legend;
    BandSpec Band;
    RatioSpec Ratio;
    std::vector<StackItemSpec> Stacks;
    std::vector<OverlaySpec> Overlays;

    static PlotSpec Simple() { return PlotSpec(); }
    PlotSpec &X(const std::string &s)
    {
        XTitle = s;
        return *this;
    }
    PlotSpec &Y(const std::string &s)
    {
        YTitle = s;
        return *this;
    }
    PlotSpec &TitleText(const std::string &s)
    {
        Title = s;
        return *this;
    }
    PlotSpec &LogY(bool on)
    {
        Theme.LogY = on;
        return *this;
    }
    PlotSpec &UseRatio(bool on)
    {
        Ratio.Enable = on;
        return *this;
    }
    PlotSpec &LegendBox(double x1, double y1, double x2, double y2, int ncol = 1)
    {
        Legend = {true, x1, y1, x2, y2, ncol};
        return *this;
    }
    PlotSpec &Stack(std::vector<StackItemSpec> v)
    {
        Stacks = std::move(v);
        return *this;
    }
    PlotSpec &Overlay(std::vector<OverlaySpec> v)
    {
        Overlays = std::move(v);
        return *this;
    }
    PlotSpec &RatioDenStack()
    {
        Ratio.DenominatorOverlayLabel.reset();
        return *this;
    }
    PlotSpec &RatioDenOverlay(const std::string &lab)
    {
        Ratio.DenominatorOverlayLabel = lab;
        return *this;
    }
};

//
struct PlanStackItem
{
    TH1 *H = nullptr;
    std::string Label;
    ColorSpec Color;
    DrawSpec Draw;
};

struct PlanOverlayItem
{
    ItemKind Kind = ItemKind::Hist;
    TH1 *H = nullptr;
    TGraph *G = nullptr;
    TGraphAsymmErrors *GAE = nullptr;
    std::string Label;
    ColorSpec Color;
    DrawSpec Draw;
    bool IsData = false;
    RatioRole Role = RatioRole::None;
};

struct RenderPlan
{
    std::vector<PlanStackItem> Stacks;
    std::vector<PlanOverlayItem> Overlays;

    TH1 *Frame = nullptr;                   //
    TH1 *StackSum = nullptr;                //
    TGraphAsymmErrors *StackBand = nullptr; //
    double YMax = 1.0;

    void Clear()
    {
        Stacks.clear();
        Overlays.clear();
        Frame = nullptr;
        StackSum = nullptr;
        StackBand = nullptr;
        YMax = 1.0;
    }
};

class PlotManager
{
  public:
    using MutateSpecHook = std::function<void(PlotSpec &)>;
    using PostRenderHook = std::function<void(TCanvas &)>;

    void OnMutateSpec(MutateSpecHook f) { m_MutateHook = std::move(f); }
    void OnPostRender(PostRenderHook f) { m_PostHook = std::move(f); }
    TCanvas *Draw(PlotSpec spec, const std::string &canvasName = "c1");

  private:
    MutateSpecHook m_MutateHook;
    PostRenderHook m_PostHook;

    // helpers
    static void ApplyStyleHist_(TH1 *h, const ColorSpec &c);
    static void ApplyStyleGraph_(TGraph *g, const ColorSpec &c);
    static void ApplyStyleGraphAsymm_(TGraphAsymmErrors *g, const ColorSpec &c);
    static void ApplyViewOps_(TH1 *h, const DrawSpec &d);

    static TH1 *MakeEmptyLike_(const TH1 *src, const char *name);
    static TH1 *MakeStackSum_(const std::vector<PlanStackItem> &stacks, const TH1 *templ);
    static TGraphAsymmErrors *MakeBandFromHist_(const TH1 *hs);
    static std::pair<TH1 *, TGraphAsymmErrors *> MakeRatio_(const TH1 *num, const TH1 *den, const char *name);

    static void SetupStyle_(const ThemeSpec &th);
    static void TuneAxes_(TH1 *f, const PlotSpec &spec, double ymin, double ymax);
    static void BuildPlan_(const PlotSpec &spec, RenderPlan &plan);
    static void ComputeYMax_(const PlotSpec &spec, RenderPlan &plan);
    static std::pair<const TH1 *, const TH1 *> FindRatioPair_(const PlotSpec &spec, const RenderPlan &plan);
    static std::string AutoLegendOpt_(const PlanOverlayItem &it);
    static bool IsTH2_(const TH1 *h) { return h && h->InheritsFrom("TH2"); }

    static inline std::string MakeSafeName(const void *p, const char *tag)
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "h%p_%s", p, tag);
        return std::string(buf);
    }
};
