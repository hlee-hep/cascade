#pragma once
#include <Math/DistFunc.h>
#include <TArrow.h>
#include <TCanvas.h>
#include <TColor.h>
#include <TFile.h>
#include <TGraph.h>
#include <TGraphAsymmErrors.h>
#include <TH1.h>
#include <TH1D.h>
#include <THStack.h>
#include <TLatex.h>
#include <TLegend.h>
#include <TLine.h>
#include <TMultiGraph.h>
#include <TObjArray.h>
#include <TObject.h>
#include <TPad.h>
#include <TROOT.h>
#include <TString.h>
#include <TStyle.h>
#include <iostream>
#include <map>
#include <set>
#include <vector>

namespace PM
{
enum Draw : uint32_t
{
    None = 0,
    kLatex = 1 << 0,
    kLegend = 1 << 1,
    kCut = 1 << 2,
    kMCerr = 1 << 3,
    kNorm = 1 << 4,
    kKtest = 1 << 5,
    kAsymErr = 1 << 6,
    kSigEmb = 1 << 7,
    kArrow = 1 << 8,
    kRatio = 1 << 9
};

}
using namespace std;

class PlotManager
{
  public:
    PlotManager();
    ~PlotManager();

    void LoadFiles(const vector<string> &files);
    void LoadFile(const string &file);
    void LoadHists();
    void SetOutputDir(const string &dir);

    map<string, THStack *> MakeStacks(const vector<string> &prefixes);
    map<string, TH1 *> ExtractPrefix(const string &prefix);
    void MergeHists(const string &newprefix, const vector<string> &prefixes);
    void SetExperiment(TString);
    void SetLuminosity(double);
    void SetExpComment(TString);

    void AddColor(const TString &);
    void AddColor(const int);

    void SetLegendAuto(const vector<int> &, const vector<TString> &);

    double SetCanvasAuto(int);

    double SetSignalScale(TH1 *, THStack *, double);
    void DrawLatex();
    void DrawLegend();

    // void DrawCutLines(double*,double ,bool );
    void PullPadSet(TPad *, TPad *, TH1 *);
    void PullPadSet(TPad *, TPad *, THStack *);

    TGraphAsymmErrors *HistToGraph(TH1 *, int);
    TGraphAsymmErrors *HistToGraph(THStack *, int);

    TObjArray *MakeRatioPlot(THStack *, TH1 *, int);
    TObjArray *MakeRatioPlot(TH1 *, TH1 *, int);

    int GetColor(int) const;

    vector<string> GetPrefixList();
    vector<string> GetVarList();

    void StyleHists(const vector<string> &prefixes,
                    const vector<int> &fillcolor, const vector<int> &linecolor);
    void StyleHists();
    void PlotSingle(const string &prefix, const string &var,
                    const string &title = "", int opts = 0);
    void PlotMultiple(const vector<vector<string>> &prefixes, const string &var,
                      const string &title = "", int opts = 0);
    void PlotComparison(const string &var, const vector<string> &prefixes_num,
                        const string &prefixes_den, const string &title = "",
                        int opts = 0);
    void SaveDrawnCanvases(const string &suffix = "pdf",
                           const string &comment = "");
    void ResetCanvases();
    string ExtractUnit(const std::string &input);

  private:
    TLatex *latex;
    TLegend *leg;
    vector<int> colors;
    TString experiment;
    TString comment;
    TString luminosity;
    vector<string> defaultcolors = {"#E24A33", "#348ABD", "#988ED5", "#777777",
                                    "#FBC15E", "#8EBA42", "#FFB5B8"};
    bool isLumCalled;
    bool isExpCalled;
    vector<unique_ptr<TH1F>> dummy;

    unordered_set<string> samples;
    string outputDir = "plots/";
    vector<TFile *> fileList;
    map<string, vector<unique_ptr<TH1>>> histMap;

    vector<pair<string, TCanvas *>> canvasList;

    vector<unique_ptr<THStack>> stackVec;
    vector<unique_ptr<TGraphAsymmErrors>> grVec;
    vector<unique_ptr<TObjArray>> arrVec;
};