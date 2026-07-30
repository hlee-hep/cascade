#include "PlotManager.hh"
#include <Math/DistFunc.h>
#include <TArrow.h>
#include <TLegendEntry.h>
#include <TList.h>
#include <TROOT.h>
#include <cmath>
#include <stdexcept>
#include "Logger.hh"
// ===== style =====
void PlotManager::ApplyStyleHist_(TH1 *h, const ColorSpec &c)
{
    if (!h) return;
    h->SetLineColor(c.Line);
    h->SetMarkerColor(c.Marker);
    h->SetMarkerStyle(c.MarkerStyle);
    h->SetMarkerSize(c.MarkerSize);
    h->SetLineWidth(c.LineWidth);
    h->SetFillColor(c.Fill);
    h->SetFillStyle(c.FillStyle);
}
void PlotManager::ApplyStyleGraph_(TGraph *g, const ColorSpec &c)
{
    if (!g) return;
    g->SetLineColor(c.Line);
    g->SetMarkerColor(c.Marker);
    g->SetMarkerStyle(c.MarkerStyle);
    g->SetMarkerSize(c.MarkerSize);
    g->SetLineWidth(c.LineWidth);
}
void PlotManager::ApplyStyleGraphAsymm_(TGraphAsymmErrors *g, const ColorSpec &c)
{
    if (!g) return;
    g->SetLineColor(c.Line);
    g->SetMarkerColor(c.Marker);
    g->SetMarkerStyle(c.MarkerStyle);
    g->SetMarkerSize(c.MarkerSize);
    g->SetLineWidth(c.LineWidth);
    g->SetFillColor(c.Fill);
    g->SetFillStyle(c.FillStyle);
}

// ===== view ops =====
void PlotManager::ApplyViewOps_(TH1 *h, const DrawSpec &d)
{
    if (!h) return;
    if (d.Rebin && *d.Rebin > 1) h->Rebin(*d.Rebin);
    if (d.Smoothing && *d.Smoothing > 0) h->Smooth(*d.Smoothing);
    if (d.Scale && *d.Scale >= 0) h->Scale(*d.Scale);

    if (d.NormBinWidth)
    {
        if (IsTH2_(h))
        {
            auto *h2 = static_cast<TH2 *>(h);
            for (int ix = 1; ix <= h2->GetNbinsX(); ++ix)
            {
                const double bwx = h2->GetXaxis()->GetBinWidth(ix);
                for (int iy = 1; iy <= h2->GetNbinsY(); ++iy)
                {
                    const double bwy = h2->GetYaxis()->GetBinWidth(iy);
                    const double bw = (bwx > 0 && bwy > 0) ? (bwx * bwy) : 1.0;
                    const double c = h2->GetBinContent(ix, iy) / bw;
                    const double e = h2->GetBinError(ix, iy) / bw;
                    h2->SetBinContent(ix, iy, c);
                    h2->SetBinError(ix, iy, e);
                }
            }
        }
        else
        {
            for (int i = 1; i <= h->GetNbinsX(); ++i)
            {
                const double bw = h->GetXaxis()->GetBinWidth(i);
                if (bw > 0)
                {
                    h->SetBinContent(i, h->GetBinContent(i) / bw);
                    h->SetBinError(i, h->GetBinError(i) / bw);
                }
            }
        }
    }
}

void PlotManager::ValidateSpec_(const PlotSpec &spec)
{
    if (spec.Layout.CanvW <= 0 || spec.Layout.CanvH <= 0)
        throw std::invalid_argument("Plot canvas dimensions must be positive");
    if (spec.Ratio.Enable && (spec.Layout.RatioSplit <= 0.0 || spec.Layout.RatioSplit >= 1.0))
        throw std::invalid_argument("Plot ratio split must be between zero and one");
    if (spec.Ratio.Enable && spec.Ratio.YMin >= spec.Ratio.YMax)
        throw std::invalid_argument("Plot ratio Y minimum must be smaller than its maximum");
    if (spec.Theme.LogY && spec.Layout.ForceYMin && spec.Layout.YMin <= 0.0)
        throw std::invalid_argument("A logarithmic plot requires a positive forced Y minimum");

    auto sameBinning = [](const TH1 *lhs, const DrawSpec &lhsDraw, const TH1 *rhs, const DrawSpec &rhsDraw)
    {
        if (!lhs || !rhs || lhs->GetDimension() != 1 || rhs->GetDimension() != 1) return false;
        if (lhsDraw.Rebin.value_or(1) != rhsDraw.Rebin.value_or(1)) return false;
        if (lhs->GetNbinsX() != rhs->GetNbinsX()) return false;
        for (int bin = 1; bin <= lhs->GetNbinsX() + 1; ++bin)
        {
            const double left = lhs->GetXaxis()->GetBinLowEdge(bin);
            const double right = rhs->GetXaxis()->GetBinLowEdge(bin);
            const double scale = std::max({1.0, std::abs(left), std::abs(right)});
            if (std::abs(left - right) > 1e-12 * scale) return false;
        }
        return true;
    };

    const StackItemSpec *stackTemplate = nullptr;
    std::size_t visibleItems = 0;
    for (const auto &stack : spec.Stacks)
    {
        if (!stack.Draw.Visible) continue;
        ++visibleItems;
        if (!stack.H) throw std::invalid_argument("Visible stack item '" + stack.Label + "' has no histogram");
        if (stack.Draw.Rebin && *stack.Draw.Rebin < 1)
            throw std::invalid_argument("Visible stack item '" + stack.Label + "' has an invalid rebin factor");
        if (stack.H->GetDimension() != 1)
            throw std::invalid_argument("Visible stack item '" + stack.Label + "' must be one-dimensional");
        if (stackTemplate && !sameBinning(stackTemplate->H, stackTemplate->Draw, stack.H, stack.Draw))
            throw std::invalid_argument("Visible stack item '" + stack.Label + "' has incompatible binning");
        if (!stackTemplate) stackTemplate = &stack;
    }

    for (const auto &overlay : spec.Overlays)
    {
        if (!overlay.Draw.Visible) continue;
        ++visibleItems;
        switch (overlay.Kind)
        {
        case ItemKind::Hist:
            if (!overlay.H) throw std::invalid_argument("Visible histogram overlay '" + overlay.Label + "' has no histogram");
            if (overlay.Draw.Rebin && *overlay.Draw.Rebin < 1)
                throw std::invalid_argument("Visible histogram overlay '" + overlay.Label + "' has an invalid rebin factor");
            break;
        case ItemKind::Graph:
            if (!overlay.G) throw std::invalid_argument("Visible graph overlay '" + overlay.Label + "' has no graph");
            break;
        case ItemKind::GraphAsymm:
            if (!overlay.GAE) throw std::invalid_argument("Visible asymmetric graph overlay '" + overlay.Label + "' has no graph");
            break;
        }
        if (overlay.Kind == ItemKind::Hist && overlay.Draw.Scale && *overlay.Draw.Scale < 0.0 && !stackTemplate)
            throw std::invalid_argument("Overlay '" + overlay.Label + "' cannot normalize to a missing stack");
    }
    if (visibleItems == 0) throw std::invalid_argument("Plot requires at least one visible item");
    if (!spec.Ratio.Enable) return;

    const OverlaySpec *numerator = nullptr;
    for (const auto &overlay : spec.Overlays)
    {
        if (overlay.Draw.Visible && overlay.Kind == ItemKind::Hist && overlay.H &&
            (overlay.IsData || overlay.Role == RatioRole::Numerator))
        {
            numerator = &overlay;
            break;
        }
    }
    if (!numerator) throw std::invalid_argument("Ratio plot requires a visible histogram numerator");
    if (numerator->H->GetDimension() != 1)
        throw std::invalid_argument("Ratio numerator '" + numerator->Label + "' must be one-dimensional");

    const TH1 *denominator = nullptr;
    const DrawSpec *denominatorDraw = nullptr;
    if (spec.Ratio.DenominatorOverlayLabel)
    {
        std::size_t matches = 0;
        for (const auto &overlay : spec.Overlays)
        {
            if (!overlay.Draw.Visible || overlay.Kind != ItemKind::Hist || !overlay.H ||
                overlay.Label != *spec.Ratio.DenominatorOverlayLabel)
                continue;
            denominator = overlay.H;
            denominatorDraw = &overlay.Draw;
            ++matches;
        }
        if (matches == 0)
            throw std::invalid_argument("Ratio denominator overlay '" + *spec.Ratio.DenominatorOverlayLabel + "' was not found");
        if (matches > 1)
            throw std::invalid_argument("Ratio denominator overlay label '" + *spec.Ratio.DenominatorOverlayLabel + "' is ambiguous");
    }
    else if (stackTemplate)
    {
        denominator = stackTemplate->H;
        denominatorDraw = &stackTemplate->Draw;
    }
    else
    {
        std::size_t matches = 0;
        for (const auto &overlay : spec.Overlays)
        {
            if (!overlay.Draw.Visible || overlay.Kind != ItemKind::Hist || !overlay.H ||
                overlay.Role != RatioRole::Denominator)
                continue;
            denominator = overlay.H;
            denominatorDraw = &overlay.Draw;
            ++matches;
        }
        if (matches > 1) throw std::invalid_argument("Ratio denominator role is ambiguous");
    }
    if (!denominator || !denominatorDraw)
        throw std::invalid_argument("Ratio plot requires a visible stack or histogram denominator");
    if (!sameBinning(numerator->H, numerator->Draw, denominator, *denominatorDraw))
        throw std::invalid_argument("Ratio numerator and denominator have incompatible binning");
}

// ===== aux creators =====
TH1 *PlotManager::MakeEmptyLike_(const TH1 *src, const char *name)
{
    TH1 *out = nullptr;
    if (src)
    {
        out = static_cast<TH1 *>(src->Clone(name));
        out->Reset("ICESM");
    }
    else
    {
        out = new TH1D(name, "", 100, 0, 1);
    }
    out->SetDirectory(nullptr);
    out->SetBit(kCanDelete, true); //
    return out;
}

TH1 *PlotManager::MakeStackSum_(const std::vector<PlanStackItem> &stacks, const TH1 *templ)
{
    if (!templ) return nullptr;
    TH1 *sum = MakeEmptyLike_(templ, "pm_stack_sum"); //
    sum->Reset("ICESM");
    for (const auto &s : stacks)
        if (s.H)
        {
            s.H->GetSumw2();
            sum->Add(s.H);
        }
    return sum;
}

TGraphAsymmErrors *PlotManager::MakeBandFromHist_(const TH1 *hs)
{
    if (!hs) return nullptr;
    const int n = hs->GetNbinsX();
    auto *g = new TGraphAsymmErrors(n);
    for (int i = 1; i <= n; ++i)
    {
        const double x = hs->GetXaxis()->GetBinCenter(i);
        double sw = hs->GetBinContent(i);
        const double sw2 = (hs->GetSumw2()->fN > 0) ? hs->GetSumw2()->At(i) : sw;
        if (sw == 0)
        {
            g->SetPoint(i - 1, x, 0);
            g->SetPointError(i - 1, 0., 0., 0., 0.);
        }
        else if (sw > 0 && sw2 > 0)
        {
            double nEff = (sw * sw) / std::max(sw2, 1e-12);
            double alpha = 1.0 - 0.682689492;
            double elow = (sw / nEff) * (nEff - ROOT::Math::gamma_quantile(alpha / 2, nEff, 1.0));
            double eup = (sw / nEff) * (ROOT::Math::gamma_quantile_c(alpha / 2., nEff + 1, 1.0) - nEff);
            const double ex = hs->GetXaxis()->GetBinWidth(i) / 2.0;
            g->SetPoint(i - 1, x, sw);
            g->SetPointError(i - 1, ex, ex, elow, eup);
        }
        else
        {
            const double error = hs->GetBinError(i);
            const double ex = hs->GetXaxis()->GetBinWidth(i) / 2.0;
            g->SetPoint(i - 1, x, sw);
            g->SetPointError(i - 1, ex, ex, error, error);
        }
    }
    g->SetBit(kCanDelete, true); //
    return g;
}

std::pair<TH1 *, TGraphAsymmErrors *> PlotManager::MakeRatio_(const TH1 *num, const TH1 *den, const char *name)
{
    if (!num || !den) return {nullptr, nullptr};
    if (num->GetDimension() != 1 || den->GetDimension() != 1) return {nullptr, nullptr};
    if (num->GetNbinsX() != den->GetNbinsX()) return {nullptr, nullptr};

    TH1 *r = MakeEmptyLike_(num, name); // kCanDelete=true
    for (int i = 1; i <= num->GetNbinsX(); ++i)
    {
        r->SetBinContent(i, num->GetBinContent(i));
        r->SetBinError(i, num->GetBinError(i));
    }
    // r->Divide(den);
    TGraphAsymmErrors *g = new TGraphAsymmErrors();
    g->SetBit(kCanDelete, true);
    g->SetLineWidth(2);
    g->Divide(r, den, "pois");
    for (int i = 0; i < g->GetN(); i++)
    {
        g->SetPointEXhigh(i, 0.0);
        g->SetPointEXlow(i, 0.0);
    }
    return {r, g};
}

// ===== style & axes =====
void PlotManager::SetupStyle_(const ThemeSpec &th)
{
    gStyle->SetCanvasBorderMode(0);
    gStyle->SetCanvasColor(kWhite);

    gStyle->SetPadBorderMode(0);
    gStyle->SetPadColor(kWhite);
    gStyle->SetPadGridX(false);
    gStyle->SetPadGridY(false);
    gStyle->SetGridColor(0);
    gStyle->SetGridStyle(3);
    gStyle->SetGridWidth(1);

    gStyle->SetFrameBorderMode(0);
    gStyle->SetFrameBorderSize(1);
    gStyle->SetFrameFillColor(0);
    gStyle->SetFrameFillStyle(0);
    gStyle->SetFrameLineColor(1);
    gStyle->SetFrameLineStyle(1);
    gStyle->SetFrameLineWidth(1);

    gStyle->SetOptStat(0);
    gStyle->SetPadTopMargin(th.PadTopMargin);
    gStyle->SetPadBottomMargin(th.PadBottomMargin);
    gStyle->SetPadLeftMargin(th.PadLeftMargin);
    gStyle->SetPadRightMargin(th.PadRightMargin);
    gStyle->SetTextFont(th.Font);
    gStyle->SetTextSize(th.TextSize);
    gStyle->SetTitleFont(th.Font, "XYZ");
    gStyle->SetLabelFont(th.Font, "XYZ");
    gStyle->SetTitleSize(th.TitleSize, "XYZ");
    gStyle->SetLabelSize(th.LabelSize, "XYZ");
    gStyle->SetLabelOffset(th.LabelOffset, "XYZ");
    gStyle->SetTitleOffset(th.TitleOffsetX, "X");
    gStyle->SetTitleOffset(th.TitleOffsetY, "Y");
    gStyle->SetPadTickX(1);
    gStyle->SetPadTickY(1);
    gStyle->SetTickLength(th.TickLengthX, "X");
    gStyle->SetTickLength(th.TickLengthY, "Y");
    gStyle->SetOptTitle(0);
    gStyle->SetErrorX(0.);
    gStyle->SetEndErrorSize(0.);
}

void PlotManager::TuneAxes_(TH1 *f, const PlotSpec &spec, double ymin, double ymax)
{
    // f->SetTitle(spec.Title.c_str());
    f->GetXaxis()->SetTitle(spec.XTitle.c_str());
    f->GetYaxis()->SetTitle(spec.YTitle.c_str());
    f->SetMinimum(ymin);
    f->SetMaximum(ymax);
    // f->GetXaxis()->SetTitleOffset(1.1);
    // f->GetYaxis()->SetTitleOffset(1.1);
    // f->Draw("AXIS");
}

// ===== plan =====
void PlotManager::BuildPlan_(const PlotSpec &spec, RenderPlan &plan)
{
    plan.Clear();

    TH1 *templ = nullptr;

    // stacks
    plan.Stacks.reserve(spec.Stacks.size());
    for (const auto &s : spec.Stacks)
    {
        if (!s.H || !s.Draw.Visible) continue;

        TH1 *h = static_cast<TH1 *>(s.H->Clone(MakeSafeName_(s.H, "stack").c_str()));
        h->SetDirectory(nullptr);
        h->SetBit(kCanDelete, false);
        plan.OwnedInputs.push_back(h);
        ApplyViewOps_(h, s.Draw);
        ApplyStyleHist_(h, s.Color);

        if (!templ) templ = h;

        PlanStackItem it;
        it.H = h;
        it.Label = s.Label;
        it.Color = s.Color;
        it.Draw = s.Draw;
        plan.Stacks.push_back(std::move(it));
    }

    // overlays
    plan.Overlays.reserve(spec.Overlays.size());
    for (const auto &o : spec.Overlays)
    {
        if (!o.Draw.Visible) continue;

        PlanOverlayItem it;
        it.Kind = o.Kind;
        it.Label = o.Label;
        it.Color = o.Color;
        it.Draw = o.Draw;
        it.IsData = o.IsData;
        it.Role = o.Role;

        if (o.Kind == ItemKind::Hist && o.H)
        {
            TH1 *h = static_cast<TH1 *>(o.H->Clone(MakeSafeName_(o.H, "overlay").c_str()));
            h->SetDirectory(nullptr);
            h->SetBit(kCanDelete, false);
            plan.OwnedInputs.push_back(h);
            ApplyViewOps_(h, o.Draw);
            ApplyStyleHist_(h, o.Color);
            if (!templ) templ = h;
            it.H = h;
        }
        else if (o.Kind == ItemKind::Graph && o.G)
        {
            it.G = static_cast<TGraph *>(o.G->Clone(MakeSafeName_(o.G, "graph").c_str()));
            it.G->SetBit(kCanDelete, false);
            plan.OwnedInputs.push_back(it.G);
            ApplyStyleGraph_(it.G, o.Color);
        }
        else if (o.Kind == ItemKind::GraphAsymm && o.GAE)
        {
            it.GAE = static_cast<TGraphAsymmErrors *>(o.GAE->Clone(MakeSafeName_(o.GAE, "graph_asymm").c_str()));
            it.GAE->SetBit(kCanDelete, false);
            plan.OwnedInputs.push_back(it.GAE);
            ApplyStyleGraphAsymm_(it.GAE, o.Color);
        }
        plan.Overlays.push_back(std::move(it));
    }

    // frame
    plan.Frame = MakeEmptyLike_(templ, "pm_frame");

    //
    bool hasTH2 = false;
    for (auto &s : plan.Stacks)
        if (IsTH2_(s.H)) hasTH2 = true;
    for (auto &o : plan.Overlays)
        if (o.H && IsTH2_(o.H)) hasTH2 = true;

    if (!hasTH2 && !plan.Stacks.empty())
    {
        plan.StackSum = MakeStackSum_(plan.Stacks, plan.Stacks.front().H);
        if (plan.StackSum)
        {
            plan.StackBand = spec.Band.Enable ? MakeBandFromHist_(plan.StackSum) : nullptr;
        }
    }

    LOG_INFO("PlotManager", "Render plan built with " << plan.Stacks.size() << " stack items and " << plan.Overlays.size()
                                                       << " overlays");
}

// ===== ymax =====
void PlotManager::ComputeYMax_(const PlotSpec &spec, RenderPlan &plan)
{
    double yMax = 0.0;
    auto up = [&](double v) { yMax = std::max(yMax, v); };

    if (plan.StackSum) up(plan.StackSum->GetMaximum() + plan.StackSum->GetBinError(plan.StackSum->GetMaximumBin()));

    for (auto &ov : plan.Overlays)
    {
        if (ov.Kind == ItemKind::Hist && ov.H) up(ov.H->GetMaximum() + ov.H->GetBinErrorUp(ov.H->GetMaximumBin()));
        if (ov.Kind == ItemKind::Graph && ov.G)
        {
            double xmin, xmax, ymin, ymax;
            ov.G->ComputeRange(xmin, xmax, ymin, ymax);
            up(ymax);
        }
        if (ov.Kind == ItemKind::GraphAsymm && ov.GAE)
        {
            double xmin, xmax, ymin, ymax;
            ov.GAE->ComputeRange(xmin, xmax, ymin, ymax);
            up(ymax);
        }
    }
    if (yMax <= 0.0) yMax = 1.0;
    yMax *= (spec.Theme.LogY ? 10.0 : 1.5);
    plan.YMax = yMax;
    LOG_INFO("PlotManager", "Computed YMax=" << yMax << (spec.Theme.LogY ? " (log scale)" : " (linear scale)"));
}

// ===== ratio helpers =====
std::pair<const TH1 *, const TH1 *> PlotManager::FindRatioPair_(const PlotSpec &spec, const RenderPlan &plan)
{
    const TH1 *num = nullptr;
    const TH1 *den = nullptr;

    for (auto &ov : plan.Overlays)
    {
        if (ov.Kind == ItemKind::Hist && ov.H && (ov.IsData || ov.Role == RatioRole::Numerator))
        {
            num = ov.H;
            break;
        }
    }
    if (!num) return {nullptr, nullptr};

    if (spec.Ratio.DenominatorOverlayLabel)
    {
        for (auto &ov : plan.Overlays)
        {
            if (ov.Kind == ItemKind::Hist && ov.H && ov.Label == *spec.Ratio.DenominatorOverlayLabel)
            {
                den = ov.H;
                break;
            }
        }
        if (!den) return {nullptr, nullptr};
    }
    else
    {
        den = plan.StackSum;
        if (!den)
        {
            for (auto &ov : plan.Overlays)
            {
                if (ov.Kind == ItemKind::Hist && ov.H && ov.Role == RatioRole::Denominator)
                {
                    den = ov.H;
                    break;
                }
            }
        }
    }
    if (!den)
    {
        LOG_INFO("PlotManager", "No valid ratio denominator found for numerator '" << num->GetName() << "'");
        return {nullptr, nullptr};
    }

    LOG_INFO("PlotManager", "Ratio pair selected. Num='" << num->GetName() << "' Den='" << den->GetName() << "'");
    return {num, den};
}

std::vector<LegendEntry> PlotManager::CollectLegendEntries_(const PlotSpec &spec, const RenderPlan &plan, bool manualMode)
{
    std::vector<LegendEntry> v;
    v.reserve(plan.Stacks.size() + plan.Overlays.size() + 1);

    auto pushStack = [&](const PlanStackItem &it)
    {
        if (!it.Draw.Visible) return;
        if (!it.Draw.VisibleInLegend) return;
        if (spec.Legend.SkipEmpty && IsEmptyObject_(it)) return;
        int prio = it.Draw.LegendPriority.value_or(0);
        v.push_back({it.Label, it.Color, "F", prio});
    };
    auto pushOverlay = [&](const PlanOverlayItem &ov)
    {
        if (!ov.Draw.Visible) return;
        if (!ov.Draw.VisibleInLegend) return;
        if (spec.Legend.SkipEmpty && IsEmptyObject_(ov)) return;

        std::string opt = (ov.Kind == ItemKind::Hist) ? (ov.Draw.LegendOption.empty() ? "PE" : ov.Draw.LegendOption)
                                                      : (ov.Draw.LegendOption.empty() ? "PE" : ov.Draw.LegendOption);
        int prio = ov.Draw.LegendPriority.value_or(0);
        v.push_back({ov.Label, ov.Color, opt, prio});
    };

    for (auto &it : plan.Stacks)
        pushStack(it);

    for (auto &ov : plan.Overlays)
        pushOverlay(ov);

    return v;
}

void PlotManager::AddLegendEntry_(TLegend *leg, const LegendEntry &e)
{
    auto *le = leg->AddEntry((TObject *)nullptr, e.Label.c_str(), e.Opt.c_str());
    if (!le) return;
    le->SetFillColor(e.Color.Fill);
    le->SetFillStyle(e.Color.FillStyle);
    le->SetLineColor(e.Color.Line);
    le->SetLineWidth(e.Color.LineWidth);
    le->SetMarkerColor(e.Color.Marker);
    le->SetMarkerStyle(e.Color.MarkerStyle);
    le->SetMarkerSize(1.0);
}
// ===== main draw =====
TCanvas *PlotManager::Draw(const PlotSpec &spec, const std::string &canvasName)
{
    // if (m_MutateHook) m_MutateHook(spec);

    //

    LOG_INFO("PlotManager", "Drawing canvas '" << canvasName << "' with " << spec.Stacks.size() << " stack specs and "
                                             << spec.Overlays.size() << " overlays");

    ValidateSpec_(spec);
    SetupStyle_(spec.Theme);

    RenderPlan plan;
    BuildPlan_(spec, plan);

    //
    /*
    for (auto &o : plan.Overlays)
    {
        if (o.H && IsTH2_(o.H))
        {
            spec.Ratio.Enable = false;
            break;
        }
    }
    for (auto &s : plan.Stacks)
    {
        if (IsTH2_(s.H))
        {
            spec.Ratio.Enable = false;
            break;
        }
    }*/

    ComputeYMax_(spec, plan);
    const double yMin = (spec.Layout.ForceYMin ? spec.Layout.YMin : (spec.Theme.LogY ? 0.1 : 0.0));
    const double yMax = (spec.Layout.ForceYMax ? spec.Layout.YMax : plan.YMax);

    // canvas & pads
    auto *canvas = new TCanvas(canvasName.c_str(), canvasName.c_str(), spec.Layout.CanvW, spec.Layout.CanvH);
    auto *ownedInputs = new TList();
    ownedInputs->SetOwner(true);
    ownedInputs->SetBit(kCanDelete, true);
    for (auto *object : plan.OwnedInputs)
        ownedInputs->Add(object);
    canvas->GetListOfPrimitives()->Add(ownedInputs);

    TPad *padTop = nullptr, *padBot = nullptr;
    const auto split = std::clamp(spec.Layout.RatioSplit, 0.05, 0.90);

    if (spec.Ratio.Enable)
    {
        padTop = new TPad((canvasName + "_top").c_str(), "", 0.0, split, 1.0, 1.0);
        padBot = new TPad((canvasName + "_bot").c_str(), "", 0.0, 0.0, 1.0, split);

        padTop->SetBottomMargin(spec.Layout.TopPadBottomMargin);
        padTop->SetTicks(1, 1);
        padBot->SetTopMargin(spec.Layout.BotPadTopMargin);
        padBot->SetBottomMargin(spec.Layout.BotPadBottomMargin);
        padBot->SetTicks(1, 1);

        padTop->Draw();
        padBot->Draw();
        padTop->cd();
    }
    else
    {
        padTop = new TPad((canvasName + "_full").c_str(), "", 0.0, 0.0, 1.0, 1.0);
        padTop->SetTicks(1, 1);
        // padTop->SetBottomMargin(spec.Layout.TopPadBottomMargin);
        padTop->Draw();
        padTop->cd();
    }
    padTop->SetBit(kCanDelete, true);
    if (padBot) padBot->SetBit(kCanDelete, true);
    if (spec.Theme.LogY) padTop->SetLogy();

    TH1 *frame = plan.Frame; // already kCanDelete=true
    frame->SetDirectory(nullptr);
    frame->SetBit(kCanDelete, true);
    // TuneAxes_(frame, spec, yMin, yMax);

    // stack
    THStack *hs = new THStack();
    hs->SetBit(kCanDelete, true);

    auto sanitizeUser = [](TObject *o)
    {
        if (!o) return;
        o->SetBit(kCanDelete, false); //
        if (auto *h = dynamic_cast<TH1 *>(o)) h->SetDirectory(nullptr);
    };

    auto safeAddToStack = [&](TObject *obj, const char *lbl)
    {
        if (!obj)
        {
            LOG_ERROR("PlotManager", "Stack item '" << lbl << "' is null");
            return;
        }
        if (!obj->InheritsFrom(TH1::Class()))
        {
            LOG_ERROR("PlotManager",
                      "Stack item '" << lbl << "' not TH1 (class=" << obj->ClassName() << ", ptr=" << (void *)obj << ")");
            return;
        }
        auto *h = static_cast<TH1 *>(obj);
        if (h->GetDimension() != 1)
        {
            LOG_ERROR("PlotManager", "Stack item '" << lbl << "' is " << h->GetDimension() << "D (THStack needs 1D)");
            return;
        }
        sanitizeUser(h);
        hs->Add(h);
    };

    for (auto &s : plan.Stacks)
        safeAddToStack(s.H, s.Label.c_str());
    if (!plan.Stacks.empty())
    {
        hs->Draw("HIST");
        TuneAxes_(hs->GetHistogram(), spec, yMin, yMax);
        hs->SetMinimum(yMin);
        hs->SetMaximum(yMax);
        hs->GetYaxis()->SetMaxDigits(4);
        if (spec.Ratio.Enable) hs->GetXaxis()->SetLabelSize(0);
        if (m_MainFrameHook) m_MainFrameHook(*(hs->GetHistogram()));
    }
    else
    {
        frame->Draw("AXIS");
        TuneAxes_(frame, spec, yMin, yMax);
        frame->GetYaxis()->SetMaxDigits(4);
        if (spec.Ratio.Enable) frame->GetXaxis()->SetLabelSize(0);
        if (m_MainFrameHook) m_MainFrameHook(*frame);
    }
    // band
    if (plan.StackBand)
    {
        auto *gb = plan.StackBand; // kCanDelete=true
        gb->SetFillStyle(spec.Band.FillStyle);
        gb->SetFillColor(spec.Band.FillColor);
        gb->SetLineColor(0);
        gb->SetMarkerSize(0);
        gb->SetFillColorAlpha(spec.Band.FillColor, spec.Band.Alpha);
        gb->Draw("E2 SAME");
    }

    // overlays
    double overlayYMax = -1;
    double cutMax = yMax / 1.45;
    double binWidth = -1;
    if (!plan.Stacks.empty()) binWidth = static_cast<TH1 *>(hs->GetStack()->Last())->GetBinWidth(1);
    for (auto &ov : plan.Overlays)
    {
        if (ov.Kind == ItemKind::Hist && ov.H)
        {
            TH1 *h = ov.H;
            sanitizeUser(h);
            const bool is2d = IsTH2_(h);
            const std::string opt = ov.Draw.DrawOpt.empty() ? (is2d ? "COLZ" : (ov.IsData ? "E1" : "HIST")) : ov.Draw.DrawOpt;
            if (ov.IsData)
            {
                h->SetBinErrorOption(TH1::kPoisson);

                if (ov.Draw.ZeroError)
                {
                    h->Draw((opt + " SAME").c_str());
                }
                else
                {
                    auto hg = new TGraphAsymmErrors(h);
                    hg->SetBit(kCanDelete, true);
                    for (int i = hg->GetN() - 1; i >= 0; i--)
                    {
                        double x, y;
                        hg->GetPoint(i, x, y);
                        int bin = h->GetXaxis()->FindBin(x);
                        if (h->GetBinContent(bin) == 0)
                        {
                            hg->RemovePoint(i);
                        }
                    }
                    hg->Draw((opt + " SAME").c_str());
                }
            }
            else
            {
                if (ov.Draw.Scale && *ov.Draw.Scale < 0 && !plan.Stacks.empty())
                {
                    const double overlayIntegral = h->Integral();
                    auto *stackTotal = hs->GetStack() ? static_cast<TH1 *>(hs->GetStack()->Last()) : nullptr;
                    if (stackTotal && overlayIntegral != 0.0)
                        h->Scale(stackTotal->Integral() / overlayIntegral);
                    else
                        LOG_WARN("PlotManager", "Cannot normalize overlay '" << h->GetName() << "' to an empty stack or zero integral.");
                }
                h->Draw((opt + " SAME").c_str());
            }

            if (h->GetMaximum() + h->GetBinErrorUp(h->GetMaximumBin()) > yMax / 1.5)
            {
                overlayYMax = h->GetMaximum() + h->GetBinErrorUp(h->GetMaximumBin());
                cutMax = (h->GetMaximum() + h->GetBinErrorUp(h->GetMaximumBin())) * 1.05;
            }

            if (binWidth < 0) binWidth = h->GetBinWidth(1);
        }
        else if (ov.Kind == ItemKind::Graph && ov.G)
        {
            sanitizeUser(ov.G);
            const std::string opt = ov.Draw.DrawOpt.empty() ? "PE" : ov.Draw.DrawOpt;
            ov.G->Draw((opt + " SAME").c_str());
        }
        else if (ov.Kind == ItemKind::GraphAsymm && ov.GAE)
        {
            sanitizeUser(ov.GAE);
            const std::string opt = ov.Draw.DrawOpt.empty() ? "PE" : ov.Draw.DrawOpt;
            ov.GAE->Draw((opt + " SAME").c_str());
        }
    }

    if (overlayYMax > 0)
    {
        if (!plan.Stacks.empty())
            hs->SetMaximum(1.5 * overlayYMax);
        else
            frame->SetMaximum(1.5 * overlayYMax);
    }

    if (spec.Cut.UpCut)
    {
        auto *cline = new TLine(*spec.Cut.UpCut, 0, *spec.Cut.UpCut, cutMax);
        auto *carrow = new TArrow(*spec.Cut.UpCut, cutMax, *spec.Cut.UpCut - spec.Cut.ArrowLength * binWidth, cutMax, 0.025, "|>");
        cline->SetLineWidth(2);
        carrow->SetLineWidth(2);
        cline->SetBit(kCanDelete, true);
        carrow->SetBit(kCanDelete, true);
        cline->Draw("SAME");
        carrow->Draw();
    }
    if (spec.Cut.DnCut)
    {
        auto *cline = new TLine(*spec.Cut.DnCut, 0, *spec.Cut.DnCut, cutMax);
        auto *carrow = new TArrow(*spec.Cut.DnCut, cutMax, *spec.Cut.DnCut + spec.Cut.ArrowLength * binWidth, cutMax, 0.025, "|>");
        cline->SetLineWidth(2);
        carrow->SetLineWidth(2);
        cline->SetBit(kCanDelete, true);
        carrow->SetBit(kCanDelete, true);
        cline->Draw("SAME");
        carrow->Draw();
    }
    // legend
    TLegend *leg = new TLegend(spec.Legend.X1, spec.Legend.Y1, spec.Legend.X2, spec.Legend.Y2);
    leg->SetBit(kCanDelete, true);
    if (spec.Legend.Enable)
    {
        leg->SetNColumns(spec.Legend.NCol);
        leg->SetBorderSize(0);
        leg->SetFillStyle(0);
        leg->SetTextFont(spec.Theme.Font);
        auto entries = CollectLegendEntries_(spec, plan, /*manualMode*/ spec.Legend.Mode == LegendMode::Manual);

        bool drawAuto = (spec.Legend.Mode == LegendMode::Auto);

        if (spec.Legend.Mode == LegendMode::Manual)
        {
            bool allHavePriority = true;
            for (auto &e : entries)
            {
            }

            for (auto &it : plan.Stacks)
            {
                if (!it.Draw.Visible) continue;
                if (!it.Draw.VisibleInLegend) continue;
                if (spec.Legend.SkipEmpty && IsEmptyObject_(it)) continue;
                if (!it.Draw.LegendPriority.has_value())
                {
                    allHavePriority = false;
                    break;
                }
            }
            if (allHavePriority)
            {
                for (auto &ov : plan.Overlays)
                {
                    if (!ov.Draw.Visible) continue;
                    if (!ov.Draw.VisibleInLegend) continue;
                    if (spec.Legend.SkipEmpty && IsEmptyObject_(ov)) continue;
                    if (!ov.Draw.LegendPriority.has_value())
                    {
                        allHavePriority = false;
                        break;
                    }
                }
            }

            if (!allHavePriority)
            {
                // auto fallback
                drawAuto = true;
            }
            else
            {
                std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) { return a.Priority < b.Priority; });
                for (auto &e : entries)
                    AddLegendEntry_(leg, e);
            }
        }

        if (drawAuto)
        {
            for (auto &ov : plan.Overlays)
            {
                if (!ov.Draw.Visible) continue;
                if (!ov.Draw.VisibleInLegend) continue;
                if (spec.Legend.SkipEmpty && IsEmptyObject_(ov)) continue;
                const std::string opt = (ov.Kind == ItemKind::Hist) ? (ov.Draw.LegendOption.empty() ? "PE" : ov.Draw.LegendOption)
                                                                    : (ov.Draw.LegendOption.empty() ? "PE" : ov.Draw.LegendOption);
                AddLegendEntry_(leg, {ov.Label, ov.Color, opt, 0});
            }
            for (auto &it : plan.Stacks)
            {
                if (!it.Draw.Visible) continue;
                if (!it.Draw.VisibleInLegend) continue;
                if (spec.Legend.SkipEmpty && IsEmptyObject_(it)) continue;
                AddLegendEntry_(leg, {it.Label, it.Color, "F", 0});
            }
        }
        if (plan.StackSum && spec.Band.Enable)
        {
            ColorSpec bc;
            bc.Fill = spec.Band.FillColor;
            bc.FillStyle = spec.Band.FillStyle;
            bc.Line = 0;
            AddLegendEntry_(leg, {spec.Band.Name.c_str(), bc, "F", 0});
        }

        leg->Draw("SAME");
        if (m_LegendHook) m_LegendHook(*leg);
    }
    else
    {
        delete leg;
        leg = nullptr;
    }
    // latex
    TLatex *latExp = new TLatex();
    TLatex *latLumi = new TLatex();
    if (spec.Sample.Enable)
    {
        latExp->SetNDC();
        latExp->SetText(spec.Sample.XExp, spec.Sample.YExp, Form("#bf{#it{%s}}  %s", spec.Sample.Experiment.c_str(), spec.Sample.Comment.c_str()));
        if (m_ExpHook) m_ExpHook(*latExp);
        latExp->SetBit(kCanDelete, true);
        latExp->Draw();
        if (spec.Sample.Lumi > 0)
        {
            latLumi->SetNDC();
            latLumi->SetText(spec.Sample.XLumi, spec.Sample.YLumi,
                             Form("#scale[0.5]{#int}#scale[0.8]{#it{L}dt = %g %s}", spec.Sample.Lumi, spec.Sample.LumiUnit.c_str()));
            if (m_LumiHook) m_LumiHook(*latLumi);
            latLumi->SetBit(kCanDelete, true);
            latLumi->Draw();
        }
        else
        {
            delete latLumi;
            latLumi = nullptr;
        }
    }
    else
    {
        delete latExp;
        delete latLumi;
        latExp = nullptr;
        latLumi = nullptr;
    }

    TGraphAsymmErrors *gr = new TGraphAsymmErrors();
    gr->SetBit(kCanDelete, true);
    bool ratioBandDrawn = false;
    // ratio
    if (spec.Ratio.Enable && padBot)
    {
        padBot->cd();
        auto pr = FindRatioPair_(spec, plan);
        if (pr.first && pr.second)
        {
            auto [r, g] = MakeRatio_(pr.first, pr.second, "pm_ratio");
            if (r)
            {
                double rScale = (1 - spec.Layout.RatioSplit) / spec.Layout.RatioSplit;
                r->SetTitle("");
                r->GetYaxis()->SetTitle(spec.Ratio.YLabel.c_str());
                r->GetYaxis()->SetNdivisions(505);
                r->GetYaxis()->SetLabelSize(rScale * spec.Theme.LabelSize);
                r->GetYaxis()->SetTitleSize(rScale * spec.Theme.TitleSize);
                r->GetYaxis()->SetTitleOffset(spec.Theme.TitleOffsetY / rScale);
                r->GetYaxis()->SetTickLength(spec.Theme.TickLengthY);
                r->GetXaxis()->SetTitle(spec.XTitle.c_str());
                r->GetXaxis()->SetLabelSize(rScale * spec.Theme.LabelSize);
                r->GetXaxis()->SetTitleSize(rScale * spec.Theme.TitleSize);
                r->GetXaxis()->SetTitleOffset(spec.Theme.TitleOffsetX);
                r->GetXaxis()->SetTickLength(rScale * spec.Theme.TickLengthX);
                r->SetMinimum(spec.Ratio.YMin);
                r->SetMaximum(spec.Ratio.YMax);
                r->SetMarkerStyle(20);
                r->SetMarkerSize(0.9);
                r->SetLineWidth(1);
                g->SetMarkerStyle(20);
                r->Draw("AXIS");
                if (spec.Ratio.MCError)
                {
                    TGraphAsymmErrors *gratio = MakeBandFromHist_(pr.second);
                    for (int i = 0; i < gratio->GetN(); i++)
                    {
                        double sw = pr.second->GetBinContent(i + 1);
                        double bwidth = pr.second->GetBinWidth(i + 1);
                        if (sw == 0)
                        {
                            gr->AddPoint(pr.second->GetBinCenter(i + 1), 1.0);
                            gr->SetPointError(i, bwidth / 2., bwidth / 2., 0, 0);
                        }
                        else
                        {
                            gr->AddPoint(pr.second->GetBinCenter(i + 1), 1.0);
                            gr->SetPointError(i, bwidth / 2., bwidth / 2., gratio->GetErrorYlow(i) / sw, gratio->GetErrorYhigh(i) / sw);
                        }
                    }
                    gr->SetLineColor(1);
                    gr->SetFillStyle(3254);
                    gr->SetFillColor(1);
                    gr->Draw("E2 SAME");
                    ratioBandDrawn = true;
                    delete gratio;
                }
                if (spec.Band.Asymm)
                {
                    g->Draw("PE SAME");
                }
                else
                {
                    r->Draw("PE SAME");
                }

                if (spec.Ratio.UnityLine)
                {
                    auto *ln = new TLine(pr.first->GetXaxis()->GetXmin(), 1.0, pr.first->GetXaxis()->GetXmax(), 1.0);
                    ln->SetLineStyle(2);
                    ln->SetLineColor(kGray + 2);
                    ln->SetBit(kCanDelete, true);
                    ln->Draw("SAME");
                }

                if (spec.Ratio.Arrow)
                {
                    for (int i = 0; i < g->GetN(); i++)
                    {
                        if (g->GetPointY(i) > spec.Ratio.YMax)
                        {
                            double xarr = g->GetPointX(i);
                            const double arrowTop = spec.Ratio.YMax;
                            const double arrowBottom = arrowTop - 0.05 * (spec.Ratio.YMax - spec.Ratio.YMin);
                            auto *arr = new TArrow(xarr, arrowBottom, xarr, arrowTop, 0.015, "|>");
                            arr->SetBit(kCanDelete, true);
                            arr->Draw();
                        }
                    }
                }
                if (m_RatioFrameHook) m_RatioFrameHook(*r);
                if (!spec.Band.Asymm) delete g;
            }
        }
        else
        {
            TH1 *ax = MakeEmptyLike_(plan.Frame, "pm_ratio_empty");
            ax->Reset("ICESM");
            ax->SetMinimum(spec.Ratio.YMin);
            ax->SetMaximum(spec.Ratio.YMax);
            ax->GetXaxis()->SetTitle(spec.XTitle.c_str());
            ax->GetYaxis()->SetTitle(spec.Ratio.YLabel.c_str());
            ax->Draw("AXIS");
            if (m_RatioFrameHook) m_RatioFrameHook(*ax);
        }
    }

    if (m_PadsHook) m_PadsHook(*padTop, padBot);

    canvas->cd();
    canvas->Update();

    if (plan.StackSum)
    {
        delete plan.StackSum;
        plan.StackSum = nullptr;
    }
    if (!ratioBandDrawn) delete gr;

    if (m_PostHook) m_PostHook(*canvas);
    LOG_INFO("PlotManager", "Finished drawing canvas '" << canvasName << "'");
    return canvas;
}
