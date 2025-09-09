#include "PlotManager.hh"
#include <Math/DistFunc.h>
#include <TLegendEntry.h>
#include <TROOT.h>
#include <cstdio>
#include <iostream>

// ===== style =====
void PlotManager::ApplyStyleHist_(TH1 *h, const ColorSpec &c)
{
    if (!h) return;
    h->SetLineColor(c.Line);
    h->SetMarkerColor(c.Marker);
    h->SetMarkerStyle(c.MarkerStyle);
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
    g->SetLineWidth(c.LineWidth);
}
void PlotManager::ApplyStyleGraphAsymm_(TGraphAsymmErrors *g, const ColorSpec &c)
{
    if (!g) return;
    g->SetLineColor(c.Line);
    g->SetMarkerColor(c.Marker);
    g->SetMarkerStyle(c.MarkerStyle);
    g->SetLineWidth(c.LineWidth);
    g->SetFillColor(c.Fill);
    g->SetFillStyle(c.FillStyle);
}

// ===== view ops =====
void PlotManager::ApplyViewOps_(TH1 *h, const DrawSpec &d)
{
    if (!h) return;
    if (d.Rebin && *d.Rebin > 1) h->Rebin(*d.Rebin);
    if (d.Scale) h->Scale(*d.Scale);

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

// ===== aux creators =====
TH1 *PlotManager::MakeEmptyLike_(const TH1 *src, const char *name)
{
    TH1 *out = nullptr;
    if (src)
    {
        if (src->GetDimension() == 1)
        {
            out = new TH1D(name, "", src->GetNbinsX(), src->GetXaxis()->GetXmin(), src->GetXaxis()->GetXmax());
        }
        else if (src->GetDimension() == 2)
        {
            auto *h2 = static_cast<const TH2 *>(src);
            out = new TH2D(name, "", h2->GetNbinsX(), h2->GetXaxis()->GetXmin(), h2->GetXaxis()->GetXmax(), h2->GetNbinsY(), h2->GetYaxis()->GetXmin(),
                           h2->GetYaxis()->GetXmax());
        }
        else
        {
            out = new TH1D(name, "", 100, 0, 1);
        }
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
        if (sw == 0)
        {
            g->SetPoint(i - 1, x, 0);
            g->SetPointError(i - 1, 0., 0., 0., 0.);
        }
        else
        {
            double sw2 = (hs->GetSumw2()->fN > 0) ? hs->GetSumw2()->At(i) : sw;
            double nEff = (sw * sw) / std::max(sw2, 1e-12);
            double alpha = 1.0 - 0.682689492;
            double elow = (sw / nEff) * (nEff - ROOT::Math::gamma_quantile(alpha / 2, nEff, 1.0));
            double eup = (sw / nEff) * (ROOT::Math::gamma_quantile_c(alpha / 2., nEff + 1, 1.0) - nEff);
            const double ex = hs->GetXaxis()->GetBinWidth(i) / 2.0;
            g->SetPoint(i - 1, x, sw);
            g->SetPointError(i - 1, ex, ex, elow, eup);
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
    g->Divide(r, den, "pois");
    return {r, g};
}

// ===== style & axes =====
void PlotManager::SetupStyle_(const ThemeSpec &th)
{
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
    gStyle->SetTitleOffset(th.TitleOffset, "XYZ");
    gStyle->SetTitleOffset(th.TitleOffset, "XYZ");
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
    //f->SetTitle(spec.Title.c_str());
    f->GetXaxis()->SetTitle(spec.XTitle.c_str());
    f->GetYaxis()->SetTitle(spec.YTitle.c_str());
    f->SetMinimum(ymin);
    f->SetMaximum(ymax);
    //f->GetXaxis()->SetTitleOffset(1.1);
    //f->GetYaxis()->SetTitleOffset(1.1);
    //f->Draw("AXIS");
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

        TH1 *h = s.H; //
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
            TH1 *h = o.H; //
            ApplyViewOps_(h, o.Draw);
            ApplyStyleHist_(h, o.Color);
            if (!templ) templ = h;
            it.H = h;
        }
        else if (o.Kind == ItemKind::Graph && o.G)
        {
            ApplyStyleGraph_(o.G, o.Color);
            it.G = o.G;
        }
        else if (o.Kind == ItemKind::GraphAsymm && o.GAE)
        {
            ApplyStyleGraphAsymm_(o.GAE, o.Color);
            it.GAE = o.GAE;
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
}

// ===== ymax =====
void PlotManager::ComputeYMax_(const PlotSpec &spec, RenderPlan &plan)
{
    double yMax = 0.0;
    auto up = [&](double v) { yMax = std::max(yMax, v); };

    if (plan.StackSum) up(plan.StackSum->GetMaximum());

    for (auto &ov : plan.Overlays)
    {
        if (ov.Kind == ItemKind::Hist && ov.H) up(ov.H->GetMaximum());
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
    yMax *= (spec.Theme.LogY ? 10.0 : 1.35);
    plan.YMax = yMax;
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
    }
    if (!den) den = plan.StackSum;
    if (!den) return {nullptr, nullptr};
    return {num, den};
}

std::string PlotManager::AutoLegendOpt_(const PlanOverlayItem &it)
{
    if (!it.Draw.LegendOption.empty()) return it.Draw.LegendOption;
    if (it.Kind == ItemKind::Hist) return it.IsData ? "P" : (it.Color.Fill ? "F" : "L");
    if (it.Kind == ItemKind::Graph) return "P";
    return "P";
}

// ===== main draw =====
TCanvas *PlotManager::Draw(PlotSpec spec, const std::string &canvasName)
{
    if (m_MutateHook) m_MutateHook(spec);

    //

    SetupStyle_(spec.Theme);

    RenderPlan plan;
    BuildPlan_(spec, plan);

    //
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
    }

    ComputeYMax_(spec, plan);
    const double yMin = (spec.Layout.ForceYMin ? spec.Layout.YMin : (spec.Theme.LogY ? 0.1 : 0.0));
    const double yMax = (spec.Layout.ForceYMax ? spec.Layout.YMax : plan.YMax);

    // canvas & pads
    auto *canvas = new TCanvas(canvasName.c_str(), canvasName.c_str(), spec.Layout.CanvW, spec.Layout.CanvH);

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
        //padTop->SetBottomMargin(spec.Layout.TopPadBottomMargin);
        padTop->Draw();
        padTop->cd();
    }
    padTop->SetBit(kCanDelete, true);
    if (padBot) padBot->SetBit(kCanDelete, true);
    if (spec.Theme.LogY) padTop->SetLogy();

    TH1 *frame = plan.Frame; // already kCanDelete=true
    frame->SetDirectory(nullptr);
    frame->SetBit(kCanDelete, true);
    //TuneAxes_(frame, spec, yMin, yMax);

    // stack
    THStack *hs = new THStack();
    hs->SetBit(kCanDelete, true);

    auto SanitizeUser = [](TObject *o)
    {
        if (!o) return;
        o->SetBit(kCanDelete, false); //
        if (auto *h = dynamic_cast<TH1 *>(o)) h->SetDirectory(nullptr);
    };

    auto SafeAddToStack = [&](TObject *obj, const char *lbl)
    {
        if (!obj)
        {
            fprintf(stderr, "[PlotManager][ERR] Stack item '%s' is null\n", lbl);
            return;
        }
        if (!obj->InheritsFrom(TH1::Class()))
        {
            fprintf(stderr, "[PlotManager][ERR] Stack item '%s' not TH1 (class=%s, ptr=%p)\n", lbl, obj->ClassName(), (void *)obj);
            return;
        }
        auto *h = static_cast<TH1 *>(obj);
        if (h->GetDimension() != 1)
        {
            fprintf(stderr, "[PlotManager][ERR] Stack item '%s' is %dD (THStack needs 1D)\n", lbl, h->GetDimension());
            return;
        }
        SanitizeUser(h);
        hs->Add(h);
    };

    for (auto &s : plan.Stacks)
        SafeAddToStack(s.H, s.Label.c_str());
    if (!plan.Stacks.empty()) 
    {
        hs->Draw("HIST");
        TuneAxes_(hs->GetHistogram(),spec,yMin,yMax);
        hs->SetMinimum(yMin);
        hs->SetMaximum(yMax);
        if (spec.Ratio.Enable) hs->GetXaxis()->SetLabelSize(0);
        if(m_MainFrameHook) m_MainFrameHook(*(hs->GetHistogram()));
    }
    else
    {
        frame->Draw("AXIS");
        TuneAxes_(frame,spec,yMin,yMax);
        if (spec.Ratio.Enable) frame->GetXaxis()->SetLabelSize(0);
        if(m_MainFrameHook) m_MainFrameHook(*frame);
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
    for (auto &ov : plan.Overlays)
    {
        if (ov.Kind == ItemKind::Hist && ov.H)
        {
            TH1 *h = ov.H;
            SanitizeUser(h);
            const bool is2d = IsTH2_(h);
            const std::string opt = ov.Draw.DrawOpt.empty() ? (is2d ? "COLZ" : (ov.IsData ? "E1" : "HIST")) : ov.Draw.DrawOpt;
            if (ov.IsData) h->SetBinErrorOption(TH1::kPoisson);
            h->Draw((opt + " SAME").c_str());
        }
        else if (ov.Kind == ItemKind::Graph && ov.G)
        {
            SanitizeUser(ov.G);
            const std::string opt = ov.Draw.DrawOpt.empty() ? "PE" : ov.Draw.DrawOpt;
            ov.G->Draw((opt + " SAME").c_str());
        }
        else if (ov.Kind == ItemKind::GraphAsymm && ov.GAE)
        {
            SanitizeUser(ov.GAE);
            const std::string opt = ov.Draw.DrawOpt.empty() ? "PE" : ov.Draw.DrawOpt;
            ov.GAE->Draw((opt + " SAME").c_str());
        }
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

        auto addLegend = [&](const char *label, const ColorSpec &c, const char *opt)
        {
            auto *e = leg->AddEntry((TObject *)nullptr, label, opt);
            if (!e) return;
            e->SetFillColor(c.Fill);
            e->SetFillStyle(c.FillStyle);
            e->SetLineColor(c.Line);
            e->SetLineWidth(c.LineWidth);
            e->SetMarkerColor(c.Marker);
            e->SetMarkerStyle(c.MarkerStyle);
            e->SetMarkerSize(1.0);
        };

        for (auto &it : plan.Stacks)
            addLegend(it.Label.c_str(), it.Color, "F");
        if (plan.StackSum && spec.Band.Enable)
        {
            ColorSpec bc;
            bc.Fill = spec.Band.FillColor;
            bc.FillStyle = spec.Band.FillStyle;
            bc.Line = 0;
            addLegend("Bkg. unc.", bc, "F");
        }
        for (auto &ov : plan.Overlays)
        {
            const std::string opt = (ov.Kind == ItemKind::Hist) ? AutoLegendOpt_(ov) : "PE";
            addLegend(ov.Label.c_str(), ov.Color, opt.c_str());
        }
        leg->Draw("SAME");
        if(m_LegendHook) m_LegendHook(*leg);
    }
    else
    {
        delete leg;
        leg = nullptr;
    }

    TGraphAsymmErrors *gr = new TGraphAsymmErrors();
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
                r->SetTitle("");
                r->GetYaxis()->SetTitle(spec.Ratio.YLabel.c_str());
                r->GetYaxis()->SetNdivisions(505);
                r->GetYaxis()->SetLabelSize(spec.Theme.LabelSize);
                r->GetYaxis()->SetTitleSize(spec.Theme.TitleSize);
                r->GetYaxis()->SetTitleOffset(0.9);
                r->GetXaxis()->SetTitle(spec.XTitle.c_str());
                r->GetXaxis()->SetLabelSize(spec.Theme.LabelSize);
                r->GetXaxis()->SetTitleSize(spec.Theme.TitleSize);
                r->GetXaxis()->SetTitleOffset(1.2);
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
                if(m_RatioFrameHook) m_RatioFrameHook(*r);
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
            if(m_RatioFrameHook) m_RatioFrameHook(*ax);
        }
    }

    if(m_PadsHook) m_PadsHook(*padTop,padBot);

    canvas->cd();
    canvas->Update();

    if (plan.StackSum)
    {
        delete plan.StackSum;
        plan.StackSum = nullptr;
    }

    if (m_PostHook) m_PostHook(*canvas);
    return canvas;
}
