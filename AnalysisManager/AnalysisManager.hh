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

enum Om
{
    Append = 0,
    Recreate = 1 << 0
};

}

class AnalysisManager
{
  public:
    AnalysisManager();
    ~AnalysisManager();

    void LoadConfig(const std::string &yamlPath);
    TChain *InitTree();
    static void GenerateConfig(TTree *tree, const std::string &yamlOut, const std::vector<std::string> &filenames);
    void AddTree(const std::string &name);
    void AddTree(TTree *tree);
    double *AddVar(const std::string &name, std::string alias);
    bool AddBranch(TTree *tree, const std::string &alias, TreeOpt::Om option);
    bool AddBranch(const std::string &treeName, const std::string &alias, TreeOpt::Om option);
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

    void InitNewHistsFromConfig(const std::string &yamlPath, const std::string &prefix);
    void InitNewHistsFromFile(const std::string &histfile);
    void AddHist(const std::string &alias, std::vector<double> binfo, const std::string &prefix);
    void AddHist(const std::string &alias, TH1 *hist, const std::string &prefix);
    void FillHists(double weight);
    void SaveHistsConfig(const std::string &yamlOut);
    void SaveHists(const std::string &outfile);

    void SetRDFInputFromConfig(const std::string &yamlPath);
    void SetRDFInputFromFile(const std::string &treename, const std::string &rootfile);

    template <typename F> void DefineRDFVar(const std::string &name, F &&func, std::vector<std::string> vars)
    {
        if (!m_UseRdf) throw std::runtime_error("RDF not initialized");
        m_RdfNode = m_RdfNode->Define(name, std::forward<F>(func), vars);
    }
    void DefineRDFVar(const std::string &name, const std::string &expr);

    template <typename F> void ApplyRDFFilter(const std::string &name, F &&func, std::vector<std::string> vars, const std::string &expr)
    {
        if (!m_UseRdf) throw std::runtime_error("RDF not initialized");
        auto it = m_RawCutExpr.find(name);
        if (it != m_RawCutExpr.end())
        {
            LOG_ERROR("AnalysisManager", "SAME name of cut exists. expr : " << it->second);
        }
        else
        {
            m_RdfNode = m_RdfNode->Filter(std::forward<F>(func), vars, name);
            LOG_INFO("AnalysisManager", "Lambda filter is applied and registered. name : " << name << " and expr : --lambda:" << expr);
            m_RawCutExpr[name] = "--lambda:" + expr;
        }
    }
    void ApplyRDFFilter(const std::string &name);
    void ApplyRDFFilter(const std::string &name, const std::string &expr);
    void ApplyRDFFilterSelected(const std::vector<std::string> &names);
    void ApplyRDFFilterAll();
    void SnapshotRDF(const std::string &treeName, const std::string &fileName, TreeOpt::Om option);
    void BookRDFHist1D(const std::string &alias, const std::string &prefix, std::vector<double> binfo);
    void SaveHistsRDF(const std::string &outfile);
    void BookRDFHistsFromConfig(const std::string &yamlPath, const std::string &prefix = "");
    void BookRDFHistsFromFile(const std::string &histfile);
    inline void DisableMT() { ROOT::DisableImplicitMT(); }
    LambdaManager *GetLambdaManager();
    std::ofstream OpenResultFile(const std::string &filename, const std::string &mode = "recreate") const;

    std::unique_ptr<AnalysisManager> Fork();
    ROOT::RDF::RNode GetIsolatedRNode();

    inline std::vector<std::string> GetAllVarNames() { return m_RdfNode->GetColumnNames(); }
    inline std::vector<std::string> GetDefinedVarNames() { return m_RdfNode->GetDefinedColumnNames(); }

    void PrintCuts();
    void PrintConfig();
    void PrintHists();
    inline bool IsRDF() const { return m_UseRdf; }
    inline double GetProgress() const { return m_Progress; }

    void WriteMetaData(const std::string &filename, const std::string &hash, const std::string &baseName, const std::string &paramJson);

    struct BranchInfo
    {
        std::string RealName;
        std::string Type;
    };

  private:
    std::map<std::string, TTree *> m_TreeMap;

    std::map<std::string, BranchInfo> m_BranchMap;
    std::map<std::string, void *> m_BranchData;
    std::map<std::string, BranchInfo> m_NewBranchMap;
    std::map<std::string, double *> m_NewBranchData;

    std::map<std::string, std::map<std::string, std::vector<double>>> m_HistMap;
    std::map<std::string, std::map<std::string, TH1 *>> m_HistData;
    std::map<std::string, std::map<std::string, std::vector<double>>> m_LoadedHistMap;
    std::map<std::string, std::map<std::string, TH1 *>> m_LoadedHistData;
    std::map<std::string, std::map<std::string, ROOT::RDF::RResultPtr<TH1>>> m_HistRdf;

    std::map<std::string, std::string> m_RawCutExpr;
    std::map<std::string, TTreeFormula *> m_CutFormulas;

    std::vector<std::string> m_InputFiles;
    std::string m_InTreeName;
    TChain *m_CurrentTree = nullptr;
    std::string ExpandAliases_(const std::string &expr) const;
    double GetNewVar_(const std::string &alias) const;
    void LoadHists_(const std::string &histfile);

    std::unique_ptr<ROOT::RDataFrame> m_RdfRaw = nullptr;
    std::optional<ROOT::RDF::RNode> m_RdfNode;
    bool m_UseRdf = false;
    std::unique_ptr<LambdaManager> m_LambdaManager = nullptr;

    std::string m_Hash;
    std::string m_Basename;
    std::string m_ParamJson;
    std::string m_Cuts;
    std::string m_EndTime;

    std::chrono::steady_clock::time_point m_StartTime;
    double m_Progress = 0.0;
    void UpdateProgress_(double p);
    std::mutex m_ProgressMutex;
};
