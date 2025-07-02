#pragma once
#include "LambdaManager.hh"
#include "Logger.hh"
#include <ROOT/RDataFrame.hxx>
#include <TChain.h>
#include <TFile.h>
#include <TH1D.h>
#include <TTree.h>
#include <TTreeFormula.h>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <yaml-cpp/yaml.h>

namespace TreeOpt
{

enum OM
{
    kAppend = 0,
    kRecreate = 1 << 0
};
}

class AnalysisManager
{
  public:
    AnalysisManager();
    ~AnalysisManager();

    void LoadConfig(const std::string &yamlPath);
    TChain *InitTree();
    static void GenerateConfig(TTree *tree, const std::string &yamlOut,
                               const std::vector<std::string> &filenames);
    void AddTree(const std::string &name);
    void AddTree(TTree *tree);
    double *AddVar(const std::string &name, std::string alias);
    bool AddBranch(TTree *tree, const std::string &alias, int option);
    bool AddBranch(const std::string &treeName, const std::string &alias,
                   int option);
    double GetVar(const std::string &alias) const;
    void LoadEntry(Long64_t);
    Long64_t GetEntries();
    void SaveTrees(const std::string &outfile);

    void LoadCuts(const std::string &yamlPath);
    void AddCut(const std::string &name, const std::string &expr);
    void ActivateSelectedCuts(const std::vector<std::string> &selected);
    void ActivateCuts();
    bool PassCut(const std::string &name) const;
    bool PassCut(const std::vector<std::string> &names);
    bool PassCut(std::initializer_list<std::string> names);
    bool PassCut();
    void SaveCuts(const std::string &yamlPath) const;
    std::string GetCutString(const std::string &name) const;
    std::vector<std::string> GetInputFiles() const;
    std::map<std::string, std::string> GetCutExpressions() const;

    void InitNewHistsFromConfig(const std::string &yamlPath,
                                const std::string &prefix);
    void InitNewHistsFromFile(const std::string &histfile);
    void AddHist(const std::string &alias, std::vector<double> binfo,
                 const std::string &prefix);
    void AddHist(const std::string &alias, TH1 *hist,
                 const std::string &prefix);
    void FillHists(double weight);
    void SaveHistsConfig(const std::string &yamlOut);
    void SaveHists(const std::string &outfile);

    void SetRDFInput(const std::string &yamlPath);

    template <typename F>
    void DefineRDFVar(const std::string &name, F &&func,
                      std::vector<std::string> vars)
    {
        if (!useRDF)
            throw std::runtime_error("RDF not initialized");
        rdf_node = rdf_node->Define(name, std::forward<F>(func), vars);
        definedVars.insert(name);
    }
    void DefineRDFVar(const std::string &name, const std::string &expr);

    template <typename F>
    void ApplyRDFFilter(const std::string &name, F &&func,
                        std::vector<std::string> vars, const std::string &expr)
    {
        if (!useRDF)
            throw std::runtime_error("RDF not initialized");
        auto it = rawCutExpr.find(name);
        if (it != rawCutExpr.end())
        {
            LOG_ERROR("AnalysisManager",
                      "SAME name of cut exists. expr : " << it->second);
        }
        else
        {
            rdf_node = rdf_node->Filter(std::forward<F>(func), vars, name);
            LOG_INFO("AnalysisManager",
                     "Lambda filter is applied and registered. name : "
                         << name << " and expr : --lambda:" << expr);
            rawCutExpr[name] = "--lambda:" + expr;
        }
    }
    void ApplyRDFFilter(const std::string &name);
    void ApplyRDFFilter(const std::string &name, const std::string &expr);
    void ApplyRDFFilterSelected(const std::vector<std::string> &names);
    void ApplyRDFFilterAll();
    void SnapshotRDF(const std::string &treeName, const std::string &fileName,
                     int option);
    void BookRDFHist1D(const std::string &alias, const std::string &prefix,
                       std::vector<double> binfo);
    void SaveHistsRDF(const std::string &outfile);
    void BookRDFHistsFromConfig(const std::string &yamlPath,
                                const std::string &prefix = "");
    void BookRDFHistsFromFile(const std::string &histfile);
    inline void DisableMT() { ROOT::DisableImplicitMT(); }
    LambdaManager *GetLambdaManager();
    std::ofstream OpenResultFile(const std::string &filename,
                                 const std::string &mode = "recreate") const;

    void PrintCuts();
    void PrintConfig();
    void PrintHists();
    inline bool IsRDF() const { return useRDF; }
    inline double GetProgress() const { return _progress; }

    void WriteMetaData(const std::string &filename, const std::string &hash,
                       const std::string &basename,
                       const std::string &paramjson);

    struct BranchInfo
    {
        std::string realName;
        std::string type;
    };

  private:
    std::map<std::string, TTree *> treeMap;

    std::map<std::string, BranchInfo> branchMap;
    std::map<std::string, void *> branchData;
    std::map<std::string, BranchInfo> newBranchMap;
    std::map<std::string, double *> newBranchData;

    std::map<std::string, std::map<std::string, std::vector<double>>> histMap;
    std::map<std::string, std::map<std::string, TH1 *>> histData;
    std::map<std::string, std::map<std::string, std::vector<double>>>
        loadedHistMap;
    std::map<std::string, std::map<std::string, TH1 *>> loadedHistData;
    std::map<std::string, std::map<std::string, ROOT::RDF::RResultPtr<TH1>>>
        histRDF;

    std::map<std::string, std::string> rawCutExpr;
    std::map<std::string, TTreeFormula *> cutFormulas;

    std::vector<std::string> inputFiles;
    std::string inTreeName;
    TChain *currentTree = nullptr;
    std::string ExpandAliases(const std::string &expr) const;
    double GetNewVar(const std::string &alias) const;
    void LoadHists(const std::string &histfile);

    std::unique_ptr<ROOT::RDataFrame> rdf_raw = nullptr;
    std::optional<ROOT::RDF::RNode> rdf_node;
    std::set<std::string> definedVars;
    bool useRDF = false;
    std::unique_ptr<LambdaManager> lm = nullptr;

    std::string _hash;
    std::string _basename;
    std::string _paramjson;
    std::string _cuts;
    std::string _endtime;

    std::chrono::steady_clock::time_point start_time;
    double _progress = 0.0;
    void UpdateProgress(double p);
    std::mutex prg_mtx;
};