#include "PlotManager.hh"
#include "Logger.hh"
#include <TKey.h>

PlotManager::PlotManager()
{
    for (auto &x : defaultcolors)
        colors.push_back(TColor::GetColor(x.c_str()));
    latex = new TLatex();
    leg = nullptr;
    TH1::AddDirectory(kFALSE);
}
PlotManager::~PlotManager()
{
    delete latex;
    delete leg;
    for (auto &file : fileList)
        delete file;
    for (auto &can : canvasList)
        delete can.second;
}

vector<string> PlotManager::GetPrefixList()
{
    return vector<string>(samples.begin(), samples.end());
}
vector<string> PlotManager::GetVarList()
{
    set<string> varset;
    for (auto &[alias, _] : histMap)
        varset.insert(alias);

    return vector<string>(varset.begin(), varset.end());
}
void PlotManager::LoadFiles(const vector<string> &files)
{
    for (auto &file : files)
    {
        TFile *f = new TFile(file.c_str());
        fileList.push_back(f);
    }
}

void PlotManager::LoadFile(const string &file)
{
    TFile *f = new TFile(file.c_str());
    fileList.push_back(f);
}

void PlotManager::LoadHists()
{
    for (auto &file : fileList)
    {
        TDirectory *dir = static_cast<TDirectory *>(file);

        TIter next(dir->GetListOfKeys());
        TKey *key;
        while ((key = static_cast<TKey *>(next())))
        {
            TObject *obj = key->ReadObj();

            if (obj->InheritsFrom(TH1::Class()))
            {
                TString parsename = obj->GetName();
                parsename = parsename.Remove(0, parsename.First('_') + 1);
                TString alias_t = parsename(0, parsename.Last('_'));
                TString prefix_t =
                    parsename(parsename.Last('_') + 1, parsename.Length());
                const char *alias = alias_t.Data();
                const char *prefix = prefix_t.Data();
                TH1 *hist = static_cast<TH1 *>(obj);
                hist->SetDirectory(nullptr);
                auto cloned =
                    std::unique_ptr<TH1>(static_cast<TH1 *>(hist->Clone()));
                histMap[alias].push_back(std::move(cloned));

                samples.insert(string(prefix));
            }
            delete obj;
        }
        file->Close();
    }
}

map<string, THStack *> PlotManager::MakeStacks(const vector<string> &prefixes)
{
    map<string, THStack *> hstacks;

    for (auto &[alias, _] : histMap)
    {
        auto hstack = make_unique<THStack>();
        hstacks[alias] = hstack.get();
        stackVec.push_back(std::move(hstack));
    }
    for (auto &prefix : prefixes)
    {
        for (auto &[alias, hists] : histMap)
        {
            for (auto &hist : hists)
            {
                TString hname = hist->GetName();
                if (hname.EndsWith(prefix.c_str()))
                    hstacks[alias]->Add(hist.get());
            }
        }
    }
    return hstacks;
}

map<string, TH1 *> PlotManager::ExtractPrefix(const string &prefix)
{
    map<string, TH1 *> hmap;
    for (auto &[alias, hists] : histMap)
    {
        for (auto &hist : hists)
        {
            TString hname = hist->GetName();
            if (hname.EndsWith(prefix.c_str()))
                hmap[alias] = hist.get();
        }
    }
    return hmap;
}

void PlotManager::StyleHists()
{
    int count = 0;
    for (auto &s : samples)
    {
        for (auto &[alias, hists] : histMap)
        {
            for (auto &hist : hists)
            {
                TString hname = hist->GetName();
                if (hname.EndsWith(s.c_str()))
                {
                    hist->SetFillColor(GetColor(count + 1));
                }
            }
        }
        count++;
    }
}

void PlotManager::StyleHists(const vector<string> &prefixes,
                             const vector<int> &fillcolor,
                             const vector<int> &linecolor)
{
    for (auto &[alias, hists] : histMap)
    {
        for (auto &hist : hists)
        {
            TString hname = hist->GetName();
            for (size_t i = 0; i < prefixes.size(); i++)
            {
                if (hname.EndsWith(prefixes[i].c_str()))
                {
                    hist->SetFillColor(GetColor(fillcolor[i]));
                    hist->SetLineColor(GetColor(linecolor[i]));
                    hist->SetLineWidth(2);
                }

                if (hname.Contains("data", TString::kIgnoreCase))
                {
                    hist->SetFillColor(0);
                    hist->SetLineColor(1);
                }
            }
        }
    }
}

void PlotManager::MergeHists(const string &newprefix,
                             const vector<string> &prefixes)
{
    samples.insert(newprefix);
    for (auto &[alias, hists] : histMap)
    {
        auto hmerged =
            unique_ptr<TH1>(static_cast<TH1 *>(histMap[alias][0]->Clone(
                Form("hist_%s_%s", alias.c_str(), newprefix.c_str()))));
        hmerged->Reset();
        hmerged->SetDirectory(nullptr);
        for (auto &hist : hists)
        {
            TString hname = hist->GetName();
            for (auto &prefix : prefixes)
            {
                if (hname.EndsWith(prefix.c_str()))
                {
                    hmerged->Add(hist.get());
                }
            }
        }
        histMap[alias].push_back(std::move(hmerged));
    }
}
void PlotManager::SetExperiment(TString str = TString(""))
{
    experiment = str;

    if (experiment.Length() == 0)
    {
        experiment = "Belle ll";
    }

    if (experiment.Contains("belle", TString::kIgnoreCase))
    {
        gROOT->LoadMacro("~/style/Belle2Style.C");
        gROOT->ProcessLine("SetBelle2Style()");
        gStyle->SetOptStat(0);
        gStyle->SetEndErrorSize(0);
        gStyle->SetLegendBorderSize(0);
    }
    isExpCalled = true;
}

void PlotManager::SetLuminosity(double lum = 0)
{
    TString invban = "fb^{-1}";
    TString temp = Form("%.1f", lum);
    if (lum < 5e-7)
    {
        isLumCalled = true;
        return;
    }
    luminosity = Form("#scale[0.85]{#bf{#int#it{L}dt = %s %s}}", temp.Data(),
                      invban.Data());
    isLumCalled = true;
}

void PlotManager::SetExpComment(TString str) { comment = str; }

void PlotManager::AddColor(const TString &str)
{
    if (!str.Contains("#"))
    {
        return;
    }

    colors.push_back(TColor::GetColor(str.Data()));
}

double PlotManager::SetCanvasAuto(int input)
{

    bool ktest = (input & PM::Draw::kKtest);
    bool mcstat = (input & PM::Draw::kMCerr);
    bool drawingcut = (input & PM::Draw::kCut);
    if (gPad == NULL)
    {

        return -256;
    }

    vector<THStack *> hs;
    vector<TH1 *> h;
    vector<TGraph *> gr;
    vector<TObject *> firstone;
    if (gPad->GetListOfPrimitives()->First() == NULL)
    {
        return -128;
    }
    TList *list = gPad->GetListOfPrimitives();
    TIter iter(list);
    for (iter.Begin(); iter != iter.End(); ++iter)
    {
        TString classname = (*iter)->ClassName();

        if (classname.Contains("TH1"))
        {
            h.push_back((TH1 *)(*iter));
            firstone.push_back(*iter);
        }
        if (classname.Contains("THStack"))
        {
            hs.push_back((THStack *)(*iter));
            firstone.push_back(*iter);
        }
        if (classname.Contains("TGraph"))
        {
            gr.push_back((TGraph *)(*iter));
            firstone.push_back(*iter);
        }
    }
    if (firstone.size() < 1)
    {
        return -512;
    }
    vector<double> maxval;
    if (!ktest)
    {
        for (int i = 0; i < hs.size(); i++)
            maxval.push_back(1.3 * hs[i]->GetMaximum());
        for (int i = 0; i < h.size(); i++)
            maxval.push_back(1.3 * h[i]->GetMaximum());
        for (int i = 0; i < gr.size(); i++)
            maxval.push_back(1.3 * gr[i]->GetMaximum());
    }
    else
    {
        for (int i = 0; i < hs.size(); i++)
            maxval.push_back(1.35 * hs[i]->GetMaximum());
        for (int i = 0; i < h.size(); i++)
            maxval.push_back(1.35 * h[i]->GetMaximum());
        for (int i = 0; i < gr.size(); i++)
            maxval.push_back(1.35 * gr[i]->GetMaximum());
    }
    if (mcstat)
    {
        double vperr = -1;
        double multf = 1.3;
        if (ktest)
            multf = 1.35;
        for (int i = 0; i < gr.size(); i++)
        {
            for (int j = 0; j < gr[i]->GetN(); j++)
            {
                double VAL = multf * (gr[i]->GetPointY(j) +
                                      abs(gr[i]->GetErrorYhigh(j)));
                if (vperr < VAL)
                    vperr = VAL;
            }
        }
        for (int i = 0; i < h.size(); i++)
        {
            double VAL = multf * (h[i]->GetMaximum() +
                                  h[i]->GetBinErrorUp(h[i]->GetMaximumBin()));
            if (vperr < VAL)
                vperr = VAL;
        }
        maxval.push_back(vperr);
    }

    auto val = max_element(std::begin(maxval), std::end(maxval));
    if (drawingcut)
        (*val) = 1.05 * (*val);

    TObject *refobj = firstone[0];
    TString refclassname = refobj->ClassName();
    if (refclassname.Contains("TH1"))
    {
        ((TH1 *)refobj)->SetMaximum(*val);
    }
    if (refclassname.Contains("THStack"))
    {
        ((THStack *)refobj)->SetMaximum(*val);
    }
    if (refclassname.Contains("TGraph"))
    {
        ((TGraph *)refobj)->SetMaximum(*val);
    }

    if (ktest)
        return (double)(*val) / 1.35;
    else
        return (double)(*val) / 1.3;
}

void PlotManager::PullPadSet(TPad *p1, TPad *p2, TH1 *pullFrame)
{
    p1->SetPad(0, 0.25, 1, 1);
    p1->SetBottomMargin(0.05);
    pullFrame->GetYaxis()->SetNdivisions(505);
    pullFrame->GetYaxis()->SetTitleSize(30);
    pullFrame->GetYaxis()->SetTitleFont(43);
    pullFrame->GetYaxis()->SetTitleOffset(1.55);
    pullFrame->GetYaxis()->SetLabelFont(
        43); // Absolute font size in pixel (precision 3)
    pullFrame->GetYaxis()->SetLabelSize(20);

    // X axis ratio plot settings
    pullFrame->GetXaxis()->SetTitleSize(30);
    pullFrame->GetXaxis()->SetTitleFont(43);
    pullFrame->GetXaxis()->SetTitleOffset(1.1);
    pullFrame->GetXaxis()->SetLabelFont(
        43); // Absolute font size in pixel (precision 3)
    pullFrame->GetXaxis()->SetLabelSize(25);
    pullFrame->GetXaxis()->SetTickLength(0.1);

    p2->SetPad(0, 0, 1, 0.25);
    p2->SetTopMargin(0.05);
    p2->SetBottomMargin(0.45);
    p1->Update();
    p2->Update();
}

void PlotManager::PullPadSet(TPad *p1, TPad *p2, THStack *pullFrame)
{
    p1->SetPad(0, 0.25, 1, 1);
    p1->SetBottomMargin(0.05);

    pullFrame->GetYaxis()->SetNdivisions(505);
    pullFrame->GetYaxis()->SetTitleSize(30);
    pullFrame->GetYaxis()->SetTitleFont(43);
    pullFrame->GetYaxis()->SetTitleOffset(1.55);
    pullFrame->GetYaxis()->SetLabelFont(
        43); // Absolute font size in pixel (precision 3)
    pullFrame->GetYaxis()->SetLabelSize(20);

    // X axis ratio plot settings
    pullFrame->GetXaxis()->SetTitleSize(30);
    pullFrame->GetXaxis()->SetTitleFont(43);
    pullFrame->GetXaxis()->SetTitleOffset(1.1);
    pullFrame->GetXaxis()->SetLabelFont(
        43); // Absolute font size in pixel (precision 3)
    pullFrame->GetXaxis()->SetLabelSize(25);
    pullFrame->GetXaxis()->SetTickLength(0.1);

    p2->SetPad(0, 0, 1, 0.25);
    p2->SetTopMargin(0.05);
    p2->SetBottomMargin(0.45);
    p1->Update();
    p2->Update();
}

void PlotManager::SetLegendAuto(const vector<int> &colorscheme,
                                const vector<TString> &components)
{
    int entries = colorscheme.size();

    if (entries > 8)
        leg = new TLegend(0.70, 0.74, 0.945, 0.93);
    else
        leg = new TLegend(0.70, 0.78, 0.945, 0.93);
    for (int i = 0; i < entries; i++)
        dummy.push_back(
            make_unique<TH1F>(Form("hdummy_%d__legend", i), "", 1, 0, 1));

    for (int i = 0; i < entries; i++)
    {
        if (components[i].Contains("sig", TString::kIgnoreCase) ||
            components[i].Contains("K#eta#nu", TString::kIgnoreCase))
        {
            dummy[i]->SetLineColor(GetColor(colorscheme[i]));
            dummy[i]->SetLineWidth(2);
            leg->AddEntry(dummy[i].get(), components[i].Data(), "F");
        }
        else if (components[i].Contains("data", TString::kIgnoreCase))
        {
            dummy[i]->SetLineColor(GetColor(colorscheme[i]));
            dummy[i]->SetMarkerColor(GetColor(colorscheme[i]));
            leg->AddEntry(dummy[i].get(), components[i].Data(), "PE");
        }
        else if (components[i].Contains("stat", TString::kIgnoreCase))
        {
            dummy[i]->SetLineWidth(0);
            dummy[i]->SetFillColor(1);
            dummy[i]->SetFillStyle(3254);
            leg->AddEntry(dummy[i].get(), components[i].Data(), "F");
        }
        else
        {
            dummy[i]->SetFillColor(GetColor(colorscheme[i]));
            dummy[i]->SetLineColor(1);
            dummy[i]->SetLineWidth(2);
            leg->AddEntry(dummy[i].get(), components[i].Data(), "F");
        }
    }
    leg->SetNColumns(2);
}

double PlotManager::SetSignalScale(TH1 *hsignal, THStack *hs,
                                   double scale = 1.0)
{
    TH1 *hbkg = (TH1 *)(hs->GetStack()->Last()->Clone(
        Form("hbkg_%ld", time(NULL) % 100000)));

    double sint = hsignal->Integral(0, -1);
    double bint = hbkg->Integral(0, -1);
    double normfactor = bint / sint;

    delete hbkg;
    return scale * normfactor;
}

void PlotManager::DrawLatex()
{
    if (!isExpCalled && !isLumCalled)
    {
        return;
    }
    latex->SetTextSize(0.045);
    if (isExpCalled)
    {
        latex->SetTextAlign(11);
        latex->DrawLatexNDC(
            0.2, 0.87,
            Form("#it{%s} #bf{%s}", experiment.Data(), comment.Data()));
    }
    if (isLumCalled)
    {
        latex->SetTextAlign(13);
        latex->DrawLatexNDC(0.2, 0.83, luminosity.Data());
    }
}

void PlotManager::DrawLegend()
{
    if (leg == NULL)
    {
        return;
    }
    leg->Draw();
}

int PlotManager::GetColor(const int i) const
{
    if (i >= (int)(colors.size()))
        return colors.at(i % colors.size());
    else if (i == -999)
        return 0;
    else if (i < 0)
        return (-i);
    else
        return colors.at(i);
}

TGraphAsymmErrors *PlotManager::HistToGraph(TH1 *h, int input)
{

    bool errbar = (input & PM::Draw::kMCerr);
    bool asymmerr = (input & PM::Draw::kAsymErr);
    bool norm = (input & PM::Draw::kNorm);

    auto grsave = make_unique<TGraphAsymmErrors>();
    auto gr = grsave.get();
    grVec.push_back(std::move(grsave));

    if (asymmerr)
    {
        for (int i = 0; i < h->GetNbinsX(); i++)
        {
            double n_eff = 0;
            double sw = 0;
            double sw2 = 0;
            double elow = 0;
            double eup = 0;
            double bwidth = h->GetBinWidth(i + 1);
            sw = h->GetBinContent(i + 1);
            sw2 = (h->GetSumw2()->fN > 0) ? h->GetSumw2()->At(i + 1) : sw * sw;
            if (sw == 0)
            {
                gr->AddPoint(h->GetBinCenter(i + 1), 0.0);
                gr->SetPointError(i, 0., 0., 0., 0.);
            }
            else
            {
                n_eff = (sw * sw) / sw2;
                double alpha = 1.0 - 0.682689492;
                elow =
                    (sw / n_eff) *
                    (n_eff - ROOT::Math::gamma_quantile(alpha / 2, n_eff, 1.0));
                eup =
                    (sw / n_eff) *
                    (ROOT::Math::gamma_quantile_c(alpha / 2., n_eff + 1, 1.0) -
                     n_eff);
                if (norm)
                {
                    gr->AddPoint(h->GetBinCenter(i + 1), 1.0);
                    if (errbar)
                        gr->SetPointError(i, bwidth / 2., bwidth / 2.,
                                          elow / sw, eup / sw);
                    else
                        gr->SetPointError(i, 0, 0, elow / sw, eup / sw);
                }
                else
                {
                    gr->AddPoint(h->GetBinCenter(i + 1), sw);
                    if (errbar)
                        gr->SetPointError(i, bwidth / 2., bwidth / 2., elow,
                                          eup);
                    else
                        gr->SetPointError(i, 0, 0, elow, eup);
                }
            }
        }
    }
    else
    {
        for (int i = 0; i < h->GetNbinsX(); i++)
        {
            if (norm) // for lower frame plot
            {
                gr->AddPoint(h->GetBinCenter(i + 1), 1.);
                if (errbar)
                    gr->SetPointError(
                        i, h->GetBinWidth(i + 1) / 2.,
                        h->GetBinWidth(i + 1) / 2.,
                        h->GetBinErrorLow(i + 1) / h->GetBinContent(i + 1),
                        h->GetBinErrorUp(i + 1) / h->GetBinContent(i + 1));
                else
                    gr->SetPointError(
                        i, 0, 0,
                        h->GetBinErrorLow(i + 1) / h->GetBinContent(i + 1),
                        h->GetBinErrorUp(i + 1) / h->GetBinContent(i + 1));
            }
            else // mc stat
            {
                gr->AddPoint(h->GetBinCenter(i + 1), h->GetBinContent(i + 1));
                if (errbar)
                    gr->SetPointError(i, h->GetBinWidth(i + 1) / 2.,
                                      h->GetBinWidth(i + 1) / 2.,
                                      h->GetBinErrorLow(i + 1),
                                      h->GetBinErrorUp(i + 1));
                else
                    gr->SetPointError(i, 0, 0, h->GetBinErrorLow(i + 1),
                                      h->GetBinErrorUp(i + 1));
            }
        }
    }
    gr->SetFillColor(1);
    gr->SetFillStyle(3254);
    return gr;
}

TGraphAsymmErrors *PlotManager::HistToGraph(THStack *hs, int input)
{

    bool errbar = (input & PM::Draw::kMCerr);
    bool asymmerr = (input & PM::Draw::kAsymErr);
    bool norm = (input & PM::Draw::kNorm);

    TH1 *h =
        (TH1 *)(hs->GetStack()->Last()->Clone(Form("mch_%s", hs->GetName())));

    auto grsave = make_unique<TGraphAsymmErrors>();
    auto gr = grsave.get();
    grVec.push_back(std::move(grsave));

    if (asymmerr)
    {
        for (int i = 0; i < h->GetNbinsX(); i++)
        {
            double n_eff = 0;
            double sw = 0;
            double sw2 = 0;
            double elow = 0;
            double eup = 0;
            double bwidth = h->GetBinWidth(i + 1);
            sw = h->GetBinContent(i + 1);
            sw2 = (h->GetSumw2()->fN > 0) ? h->GetSumw2()->At(i + 1) : sw * sw;

            if (sw == 0)
            {
                gr->AddPoint(h->GetBinCenter(i + 1), 0.0);
                gr->SetPointError(i, 0., 0., 0., 0.);
            }
            else
            {
                n_eff = (sw * sw) / sw2;
                double alpha = 1.0 - 0.682689492;
                elow =
                    (sw / n_eff) *
                    (n_eff - ROOT::Math::gamma_quantile(alpha / 2, n_eff, 1.0));
                eup =
                    (sw / n_eff) *
                    (ROOT::Math::gamma_quantile_c(alpha / 2., n_eff + 1, 1.0) -
                     n_eff);

                if (norm)
                {
                    gr->AddPoint(h->GetBinCenter(i + 1), 1.0);
                    if (errbar)
                        gr->SetPointError(i, bwidth / 2., bwidth / 2.,
                                          elow / sw, eup / sw);
                    else
                        gr->SetPointError(i, 0, 0, elow / sw, eup / sw);
                }
                else
                {
                    gr->AddPoint(h->GetBinCenter(i + 1), sw);
                    if (errbar)
                        gr->SetPointError(i, bwidth / 2., bwidth / 2., elow,
                                          eup);
                    else
                        gr->SetPointError(i, 0, 0, elow, eup);
                }
            }
        }
    }
    else
    {
        for (int i = 0; i < h->GetNbinsX(); i++)
        {
            if (norm) // for lower frame plot
            {
                gr->AddPoint(h->GetBinCenter(i + 1), 1.);
                if (errbar)
                    gr->SetPointError(
                        i, h->GetBinWidth(i + 1) / 2.,
                        h->GetBinWidth(i + 1) / 2.,
                        h->GetBinErrorLow(i + 1) / h->GetBinContent(i + 1),
                        h->GetBinErrorUp(i + 1) / h->GetBinContent(i + 1));
                else
                    gr->SetPointError(
                        i, 0, 0,
                        h->GetBinErrorLow(i + 1) / h->GetBinContent(i + 1),
                        h->GetBinErrorUp(i + 1) / h->GetBinContent(i + 1));
            }
            else // mc stat
            {
                gr->AddPoint(h->GetBinCenter(i + 1), h->GetBinContent(i + 1));
                if (errbar)
                    gr->SetPointError(i, h->GetBinWidth(i + 1) / 2.,
                                      h->GetBinWidth(i + 1) / 2.,
                                      h->GetBinErrorLow(i + 1),
                                      h->GetBinErrorUp(i + 1));
                else
                    gr->SetPointError(i, 0, 0, h->GetBinErrorLow(i + 1),
                                      h->GetBinErrorUp(i + 1));
            }
        }
    }
    gr->SetFillColor(1);
    gr->SetFillStyle(3254);
    delete h;
    return gr;
}

/*
void PlotManager::DrawCutLines(double* where, double max,bool doubleside)
{
    if(!gPad)
    {
        return;
    }

    if(!doubleside)
    {
        TLine *line = new TLine(where[0],0,where[0],max);
        line->SetLineWidth(3);
        line->SetLineColor(28);
        line->Draw("SAME");
    }
    else
    {
        TLine *line0 = new TLine(where[0],0,where[0],max);
        TLine *line1 = new TLine(where[1],0,where[1],max);

        line0->SetLineWidth(3);
        line0->SetLineColor(28);
        line1->SetLineWidth(3);
        line1->SetLineColor(28);

        line0->Draw("SAME");
        line1->Draw("SAME");
    }
}
*/

TObjArray *PlotManager::MakeRatioPlot(THStack *hmc, TH1 *hdata, int input)
{

    bool mcerrbar = (input & PM::Draw::kMCerr);
    bool asymmerr = (input & PM::Draw::kAsymErr);
    bool arrow = (input & PM::Draw::kArrow);
    int asymerrbit = int(asymmerr) << 6;
    if (!gPad)
    {
        return nullptr;
    }

    auto arrsave = make_unique<TObjArray>(4);
    TObjArray *arr = arrsave.get();
    arrVec.push_back(std::move(arrsave));
    hmc->GetXaxis()->SetLabelSize(0);

    TH1 *hmctot = (TH1 *)(hmc->GetStack()->Last()->Clone(
        Form("mctotof_%s", hdata->GetName())));
    TH1 *hratio = (TH1 *)hdata->Clone(Form("mcdataratio_%s", hdata->GetName()));
    TH1 *hdatatot = (TH1 *)hdata->Clone(Form("datatotof_%s", hdata->GetName()));

    TGraphAsymmErrors *ratiographerr = new TGraphAsymmErrors();

    TGraphAsymmErrors *mcerrgraph;

    if (!hratio->Divide(hmctot))
    {
        return nullptr;
    }
    else
    {
        arr->Add(hratio);
        ratiographerr->Divide(hdatatot, hmctot, "pois");
        arr->Add(ratiographerr);
        if (mcerrbar)
        {
            mcerrgraph = HistToGraph(hmctot, PM::Draw::kMCerr | asymerrbit |
                                                 PM::Draw::kNorm);
            mcerrgraph->SetFillColor(1);
            mcerrgraph->SetFillStyle(3254);
            arr->Add(mcerrgraph);
        }
    }

    if (arrow)
    {
        vector<double> vecArrow;
        for (int i = 0; i < ratiographerr->GetN(); i++)
        {
            if (ratiographerr->GetPointY(i) > 2)
                vecArrow.push_back(ratiographerr->GetPointX(i));
        }
        TObjArray *arrowArr = new TObjArray(vecArrow.size());

        for (int i = 0; i < vecArrow.size(); i++)
            arrowArr->Add(
                new TArrow(vecArrow[i], 1.95, vecArrow[i], 2, 0.015, "|>"));

        arr->Add(arrowArr);
    }
    for (int i = 0; i < ratiographerr->GetN(); i++)
    {
        ratiographerr->SetPointEXhigh(i, 0.0);
        ratiographerr->SetPointEXlow(i, 0.0);
    }

    hratio->GetYaxis()->SetRangeUser(0, 2);
    delete hmctot;
    delete hdatatot;
    return arr;
}

TObjArray *PlotManager::MakeRatioPlot(TH1 *hmc, TH1 *hdata, int input)
{

    bool mcerrbar = (input & PM::Draw::kMCerr);
    bool asymmerr = (input & PM::Draw::kAsymErr);
    bool arrow = (input & PM::Draw::kArrow);
    int asymerrbit = int(asymmerr) << 6;

    if (!gPad)
    {
        return nullptr;
    }
    auto arrsave = make_unique<TObjArray>(4);
    TObjArray *arr = arrsave.get();
    arrVec.push_back(std::move(arrsave));
    hmc->GetXaxis()->SetLabelSize(0);

    TH1 *hmctot = (TH1 *)(hmc->Clone(Form("mctotof_%s", hdata->GetName())));
    TH1 *hratio = (TH1 *)hdata->Clone(Form("mcdataratio_%s", hdata->GetName()));
    TH1 *hdatatot = (TH1 *)hdata->Clone(Form("datatotof_%s", hdata->GetName()));

    TGraphAsymmErrors *ratiographerr = new TGraphAsymmErrors();

    TGraphAsymmErrors *mcerrgraph;

    if (!hratio->Divide(hmctot))
    {
        return nullptr;
    }
    else
    {
        arr->Add(hratio);
        ratiographerr->Divide(hdatatot, hmctot, "pois");
        arr->Add(ratiographerr);

        if (mcerrbar)
        {
            mcerrgraph = HistToGraph(hmctot, PM::Draw::kMCerr | asymerrbit |
                                                 PM::Draw::kNorm);
            mcerrgraph->SetFillColor(1);
            mcerrgraph->SetFillStyle(3254);
            arr->Add(mcerrgraph);
        }
    }

    if (arrow)
    {
        vector<double> vecArrow;
        for (int i = 0; i < ratiographerr->GetN(); i++)
        {
            if (ratiographerr->GetPointY(i) > 2)
                vecArrow.push_back(ratiographerr->GetPointX(i));
        }
        TObjArray *arrowArr = new TObjArray(vecArrow.size());

        for (int i = 0; i < vecArrow.size(); i++)
            arrowArr->Add(
                new TArrow(vecArrow[i], 1.95, vecArrow[i], 2, 0.015, "|>"));

        arr->Add(arrowArr);
    }
    for (int i = 0; i < ratiographerr->GetN(); i++)
    {
        ratiographerr->SetPointEXhigh(i, 0.0);
        ratiographerr->SetPointEXlow(i, 0.0);
    }

    hratio->GetYaxis()->SetRangeUser(0, 2);
    delete hmctot;
    delete hdatatot;
    return arr;
}

void PlotManager::PlotSingle(const string &prefix, const string &var,
                             const string &title, int opts)
{
    TH1 *h = nullptr;
    auto it = histMap.find(var);
    if (it != histMap.end())
    {
        for (auto &hist : it->second)
        {
            TString hname = hist->GetName();
            if (hname.EndsWith(prefix.c_str()))
                h = hist.get();
        }
    }
    else
    {
        return;
    }

    TGraphAsymmErrors *errgr = HistToGraph(h, opts);
    bool norm = (opts & PM::Draw::kNorm);

    if (norm)
        h->Scale(1. / h->Integral());
    TCanvas *c1 = new TCanvas(Form("c_%s_%s", var.c_str(), prefix.c_str()), "");
    c1->cd(1);
    h->SetTitle(Form(";%s;Candidates per %g %s",
                     (title.length() == 0) ? var.c_str() : title.c_str(),
                     h->GetBinWidth(1), ExtractUnit(title).c_str()));
    h->Draw("HIST");
    if (opts & PM::Draw::kMCerr)
        errgr->Draw("E2,SAME");
    SetCanvasAuto(opts);
    if (opts & PM::Draw::kLatex)
        DrawLatex();
    if (opts & PM::Draw::kLegend)
        DrawLegend();
    c1->Update();
    canvasList.push_back({var, c1});
}

void PlotManager::PlotMultiple(const vector<vector<string>> &prefixes,
                               const string &var, const string &title, int opts)
{
    map<string, TH1 *> hsignals;
    TH1 *hsignal = nullptr;
    vector<TObject *> hists;
    map<string, TH1 *> allhists;
    map<string, THStack *> allstacks;
    double binwidth = -1;
    for (auto &all : prefixes)
    {
        if (all.size() == 1)
        {
            allhists = ExtractPrefix(all[0]);
            auto it = allhists.find(var);
            if (it != allhists.end())
            {
                hists.push_back(it->second);
                if (binwidth < 0)
                    binwidth = it->second->GetBinWidth(1);
            }
        }
        else
        {
            allstacks = MakeStacks(all);
            auto it = allstacks.find(var);
            if (it != allstacks.end())
            {
                hists.push_back(it->second);
                if (binwidth < 0)
                    binwidth =
                        static_cast<TH1 *>(it->second->GetStack()->Last())
                            ->GetBinWidth(1);
            }
        }
    }
    if (opts & PM::Draw::kSigEmb)
    {
        hsignals = ExtractPrefix("signal");
        hsignal = hsignals[var];
        if (!hsignal)
            return;
        hsignal->Scale(1);
    }

    TCanvas *c1 = new TCanvas(Form("c_%s_%d_mutiple", var.c_str(), opts), "");
    c1->cd(1);
    bool first = true;
    for (auto &h : hists)
    {
        if (first)
        {
            (h->InheritsFrom("TH1"))
                ? static_cast<TH1 *>(h)->SetTitle(
                      Form(";%s;Candidates per %g %s",
                           (title.length() == 0) ? var.c_str() : title.c_str(),
                           binwidth, ExtractUnit(title).c_str()))
                : static_cast<THStack *>(h)->SetTitle(
                      Form(";%s;Candidates per %g %s",
                           (title.length() == 0) ? var.c_str() : title.c_str(),
                           binwidth, ExtractUnit(title).c_str()));

            h->Draw("HIST");
            first = false;
        }
        else
            h->Draw("HIST,SAME");
    }
    hsignal->Scale(SetCanvasAuto(opts) / hsignal->GetMaximum());
    if (opts & PM::Draw::kSigEmb)
        hsignal->Draw("HIST,SAME");
    if (opts & PM::Draw::kLatex)
        DrawLatex();
    if (opts & PM::Draw::kLegend)
        DrawLegend();
    canvasList.push_back({var, c1});
}
void PlotManager::PlotComparison(const string &var,
                                 const vector<string> &prefixes_num,
                                 const string &prefix_den, const string &title,
                                 int opts)
{
    LOG_INFO("PlotManager", "Comparison plot is generated.");
    map<string, TH1 *> hsignals;
    TH1 *hsignal = nullptr;
    double ktestprob = -1;
    TLatex *ktest = new TLatex();
    map<string, THStack *> allhists = MakeStacks(prefixes_num);
    THStack *hbkg = allhists[var];
    map<string, TH1 *> hdatas = ExtractPrefix(prefix_den);
    TH1 *hdata = hdatas[var];
    hdata->SetLineColor(1);
    hdata->SetMarkerColor(1);

    if (!hdata || !hbkg)
        return;

    if (opts & PM::Draw::kSigEmb)
    {
        hsignals = ExtractPrefix("signal");
        hsignal = hsignals[var];
        if (!hsignal)
            return;
        hsignal->Scale(SetSignalScale(hsignal, hbkg));
    }
    TGraphAsymmErrors *mcerrgr = HistToGraph(hbkg, opts);

    TCanvas *c1 =
        new TCanvas(Form("c_%s_%d_compare", var.c_str(), opts), "", 800, 700);
    if (opts & PM::Draw::kRatio)
    {
        c1->Divide(1, 2);
        TPad *p1 = (TPad *)(c1->cd(1));
        TPad *p2 = (TPad *)(c1->cd(2));
        p1->cd();
        hbkg->SetTitle(Form(";;Candidates per %g %s", hdata->GetBinWidth(1),
                            ExtractUnit(title).c_str()));
        hbkg->Draw("HIST");
        if (opts & PM::Draw::kSigEmb)
            hsignal->Draw("HIST,SAME");
        hdata->Draw("PE,SAME");
        if (opts & PM::Draw::kMCerr)
            mcerrgr->Draw("E2,SAME");
        SetCanvasAuto(opts);
        if (opts & PM::Draw::kKtest)
        {
            ktestprob =
                ((TH1 *)(hbkg->GetStack()->Last()))->KolmogorovTest(hdata);
            ktest->SetTextSize(0.03);
            ktest->SetTextAlign(13);
            ktest->DrawLatexNDC(
                0.2, 0.76, Form("Kolmogorov Test p-Value = %.3lf", ktestprob));
        }
        if (opts & PM::Draw::kLatex)
            DrawLatex();
        if (opts & PM::Draw::kLegend)
            DrawLegend();

        p2->cd();
        TObjArray *arr = MakeRatioPlot(hbkg, hdata, opts);
        static_cast<TH1 *>((*arr)[0])->SetTitle(
            Form(";%s;Data/MC",
                 (title.length() == 0) ? var.c_str() : title.c_str()));
        (*arr)[0]->Draw("AXIS");
        (*arr)[1]->Draw("SAME,PE");
        if (opts & PM::Draw::kMCerr)
            (*arr)[2]->Draw("SAME,E2");

        if (opts & PM::Draw::kArrow)
        {
            TObjArray *arrows = (TObjArray *)(*arr)[3];
            for (Int_t i = 0; i < arrows->GetEntries(); i++)
                (*arrows)[i]->Draw();
        }
        PullPadSet(p1, p2, (TH1 *)((*arr)[0]));
        c1->Update();
    }
    else
    {
        c1->SetWindowSize(800, 600);
        c1->cd(1);
        hbkg->SetTitle(Form(";%s;Candidates per %g %s",
                            (title.length() == 0) ? var.c_str() : title.c_str(),
                            hdata->GetBinWidth(1), ExtractUnit(title).c_str()));
        hbkg->Draw("HIST");
        if (opts & PM::Draw::kSigEmb)
            hsignal->Draw("HIST,SAME");
        hdata->Draw("PE,SAME");
        if (opts & PM::Draw::kMCerr)
            mcerrgr->Draw("SAME,E2");
        SetCanvasAuto(opts);
        if (opts & PM::Draw::kKtest)
        {
            ktestprob =
                ((TH1 *)(hbkg->GetStack()->Last()))->KolmogorovTest(hdata);
            ktest->SetTextSize(0.03);
            ktest->SetTextAlign(13);
            ktest->DrawLatexNDC(
                0.2, 0.76, Form("Kolmogorov Test p-Value = %.3lf", ktestprob));
        }
        if (opts & PM::Draw::kLatex)
            DrawLatex();
        if (opts & PM::Draw::kLegend)
            DrawLegend();
        c1->Update();
    }

    canvasList.push_back({var, c1});
}

void PlotManager::RegisterCanvas(const string &name, TCanvas* c)
{
    canvasList.push_back({name,c});
}

void PlotManager::SaveDrawnCanvases(const string &suffix, const string &comment)
{
    for (auto &can : canvasList)
    {
        if (comment.length() == 0)
            can.second->Print(Form("%s/%s.%s", outputDir.c_str(),
                                   can.first.c_str(), suffix.c_str()));
        else
            can.second->Print(Form("%s/%s_%s.%s", outputDir.c_str(),
                                   can.first.c_str(), comment.c_str(),
                                   suffix.c_str()));
    }
}
void PlotManager::SetOutputDir(const string &dir) { outputDir = dir; }

void PlotManager::ResetCanvases()
{
    for (auto &can : canvasList)
        delete can.second;

    vector<pair<string, TCanvas *>>().swap(canvasList);
}

string PlotManager::ExtractUnit(const string &input)
{
    size_t start = input.find('[');
    size_t end = input.find(']');

    if (start == string::npos || end == string::npos || end <= start)
        return "";

    string unit = input.substr(start + 1, end - start - 1);

    size_t first = unit.find_first_not_of(" \t");
    size_t last = unit.find_last_not_of(" \t");

    if (first == string::npos || last == string::npos)
        return "";

    return unit.substr(first, last - first + 1);
}