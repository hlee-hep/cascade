#pragma once

#include <TCanvas.h>
#include <TGraph.h>
#include <TGraphAsymmErrors.h>
#include <TH1.h>
#include <TH2.h>
#include <THStack.h>
#include <TLatex.h>
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

enum class LegendMode
{
    Auto,
    Manual
};

struct ColorSpec
{
    int Line = 1;
    int Fill = 0;
    int Marker = 1;
    int MarkerStyle = 20;
    int LineWidth = 2;
    int FillStyle = 1001;
    float MarkerSize = 0.9;
    ColorSpec() = default;
    ColorSpec(int line, int fill, int marker, int markerStyle = 20, int lineWidth = 2, int fillStyle = 1001)
        : Line(line), Fill(fill), Marker(marker), MarkerStyle(markerStyle), LineWidth(lineWidth), FillStyle(fillStyle)
    {
    }

    // fluent setters
    ColorSpec &SetLine(int v)
    {
        Line = v;
        return *this;
    }
    ColorSpec &SetFill(int v)
    {
        Fill = v;
        return *this;
    }
    ColorSpec &SetMarker(int v)
    {
        Marker = v;
        return *this;
    }
    ColorSpec &SetMarkerStyle(int v)
    {
        MarkerStyle = v;
        return *this;
    }
    ColorSpec &SetLineWidth(int v)
    {
        LineWidth = v;
        return *this;
    }
    ColorSpec &SetFillStyle(int v)
    {
        FillStyle = v;
        return *this;
    }
    ColorSpec &SetMarkerSize(float v)
    {
        MarkerSize = v;
        return *this;
    }
    ColorSpec &Set(int line, int fill, int marker, int markerStyle = 20, int lineWidth = 2, int fillStyle = 1001)
    {
        Line = line;
        Fill = fill;
        Marker = marker;
        MarkerStyle = markerStyle;
        LineWidth = lineWidth;
        FillStyle = fillStyle;
        return *this;
    }
};
struct CutSpec
{
    std::optional<double> UpCut;
    std::optional<double> DnCut;
    double ArrowLength = 1.0; // bin
};

struct SampleSpec
{
    bool Enable = true;
    std::string Experiment = "Belle II";
    std::string Comment = "";
    double Lumi = 427.9;
    std::string LumiUnit = "fb^{-1}";
    double XExp = 0.155, YExp = 0.92, XLumi = 0.77, YLumi = 0.92;
};

struct DrawSpec
{
    std::string DrawOpt = "";
    std::optional<int> Rebin;
    std::optional<int> Smoothing;
    std::optional<double> Scale;
    bool NormBinWidth = false;
    bool Visible = true;
    bool ZeroError = true; //  only for data
    bool VisibleInLegend = true;
    std::string LegendOption;
    std::optional<int> LegendPriority;
    DrawSpec() = default;

    // fluent setters
    DrawSpec &SetOpt(std::string v)
    {
        DrawOpt = std::move(v);
        return *this;
    }

    DrawSpec &SetRebin(int r)
    {
        Rebin = r;
        return *this;
    }

    DrawSpec &SetSmoothing(int s)
    {
        Smoothing = s;
        return *this;
    }

    DrawSpec &ClearRebin()
    {
        Rebin.reset();
        return *this;
    }

    DrawSpec &SetScale(double s)
    {
        Scale = s;
        return *this;
    }
    DrawSpec &ClearScale()
    {
        Scale.reset();
        return *this;
    }

    DrawSpec &SetNormBinWidth(bool b = true)
    {
        NormBinWidth = b;
        return *this;
    }
    DrawSpec &SetVisible(bool v = true)
    {
        Visible = v;
        return *this;
    }
    DrawSpec &SetVisibleInLegend(bool b = true)
    {
        VisibleInLegend = b;
        return *this;
    }
    DrawSpec &SetLegendOpt(std::string v)
    {
        LegendOption = std::move(v);
        return *this;
    }
    DrawSpec &SetLegendPriority(int a)
    {
        LegendPriority = a;
        return *this;
    }
    DrawSpec &SetZeroError(bool b = true)
    {
        ZeroError = b;
        return *this;
    }
};

struct StackItemSpec
{
    TH1 *H = nullptr; //
    std::string Label;
    ColorSpec Color;
    DrawSpec Draw;

    StackItemSpec() = default;
    StackItemSpec(TH1 *h, std::string lab, ColorSpec c, DrawSpec d = {}) : H(h), Label(std::move(lab)), Color(std::move(c)), Draw(std::move(d)) {}

    // fluent setters
    StackItemSpec &SetHist(TH1 *h)
    {
        H = h;
        return *this;
    }
    StackItemSpec &SetLabel(std::string v)
    {
        Label = std::move(v);
        return *this;
    }
    StackItemSpec &SetColor(const ColorSpec &c)
    {
        Color = c;
        return *this;
    }
    StackItemSpec &SetDraw(const DrawSpec &d)
    {
        Draw = d;
        return *this;
    }
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
    LegendMode Mode = LegendMode::Auto;
    bool SkipEmpty = true;
};
struct LegendEntry
{
    std::string Label;
    ColorSpec Color;
    std::string Opt;
    int Priority;
};
struct RatioSpec
{
    bool Enable = false;
    std::string YLabel = "Data/MC";
    double YMin = 0.0, YMax = 2.0;
    bool UnityLine = true;
    bool MCError = true;
    bool Arrow = true;
    std::optional<std::string> DenominatorOverlayLabel; //
};

struct ThemeSpec // global
{
    int Font = 42;
    float TextSize = 0.05;
    float LabelSize = 0.054f;
    float TitleSize = 0.06f;
    float TitleOffsetX = 1.00f;
    float TitleOffsetY = 1.25f;
    float LabelOffset = 0.015f;
    float TickLengthY = 0.02;
    float TickLengthX = 0.03;
    float PadTopMargin = 0.1;
    float PadRightMargin = 0.03;
    float PadLeftMargin = 0.15;
    float PadBottomMargin = 0.15;
    bool LogY = false;
};

struct BandSpec
{
    bool Enable = true;
    bool Asymm = true;
    int FillStyle = 3254;
    int FillColor = 1;
    float Alpha = 1;
    std::string Name = "Stat. err.";
};

struct LayoutSpec // not global
{
    int CanvW = 800, CanvH = 800;
    double RatioSplit = 0.25;
    double TopPadBottomMargin = 0.05;
    double BotPadTopMargin = 0.05;
    double BotPadBottomMargin = 0.42;
    bool ForceYMin = false;
    double YMin = 0.1;
    bool ForceYMax = false;
    double YMax = 0.0;
};

struct PlotSpec
{
    std::string XTitle, YTitle = "Events";
    ThemeSpec Theme;
    LayoutSpec Layout;
    LegendSpec Legend;
    BandSpec Band;
    RatioSpec Ratio;
    CutSpec Cut;
    SampleSpec Sample;
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
    PlotSpec &LegendBox(double x1, double y1, double x2, double y2, int ncol = 1, LegendMode mode = LegendMode::Auto, bool skip = false)
    {
        Legend = {true, x1, y1, x2, y2, ncol, mode, skip};
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
    // using MutateSpecHook = std::function<void(PlotSpec &)>;
    using PostRenderHook = std::function<void(TCanvas &)>;
    using LegendHook = std::function<void(TLegend &)>;
    using PadsHook = std::function<void(TPad &, TPad *)>;
    using FrameHook = std::function<void(TH1 &)>;
    using LatexHook = std::function<void(TLatex &)>;
    // void OnMutateSpec(MutateSpecHook f) { m_MutateHook = std::move(f); }
    void OnPostRender(PostRenderHook f) { m_PostHook = std::move(f); }
    void OnLegend(LegendHook f) { m_LegendHook = std::move(f); }
    void OnExp(LatexHook f) { m_ExpHook = std::move(f); }
    void OnLumi(LatexHook f) { m_LumiHook = std::move(f); }
    void OnPads(PadsHook f) { m_PadsHook = std::move(f); }
    void OnMainFrame(FrameHook f) { m_MainFrameHook = std::move(f); }
    void OnRatioFrame(FrameHook f) { m_RatioFrameHook = std::move(f); }
    TCanvas *Draw(const PlotSpec &spec, const std::string &canvasName = "c1");

  private:
    // MutateSpecHook m_MutateHook;
    PostRenderHook m_PostHook;
    LegendHook m_LegendHook;
    LatexHook m_ExpHook;
    LatexHook m_LumiHook;
    PadsHook m_PadsHook;
    FrameHook m_MainFrameHook;
    FrameHook m_RatioFrameHook;
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

    static void AddLegendEntry_(TLegend *leg, const LegendEntry &e);
    static std::vector<LegendEntry> CollectLegendEntries_(const PlotSpec &spec, const RenderPlan &plan, bool manualMode);

    static inline bool IsEmptyObject_(const PlanStackItem &s)
    {
        if (!s.H) return true;
        return (s.H->GetEntries() == 0);
    }
    static inline bool IsEmptyObject_(const PlanOverlayItem &o)
    {
        if (o.Kind == ItemKind::Hist)
        {
            if (!o.H) return true;
            return (o.H->GetEntries() == 0);
        }
        else if (o.Kind == ItemKind::Graph)
        {
            return (!o.G || o.G->GetN() == 0);
        }
        else
        { // GraphAsymm
            return (!o.GAE || o.GAE->GetN() == 0);
        }
    }

    static inline std::string MakeSafeName_(const void *p, const char *tag)
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "h%p_%s", p, tag);
        return std::string(buf);
    }
};
