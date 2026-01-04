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

    void LoadInputConfig(const std::string &yamlPath);
    TChain *BuildChain();
    static void WriteInputConfig(TTree *tree, const std::string &yamlOut, const std::vector<std::string> &filenames);
    void RegisterTree(const std::string &name);
    void RegisterTree(TTree *tree);
    double *RegisterVariable(const std::string &name, std::string alias);
    bool AttachBranch(TTree *tree, const std::string &alias, TreeOpt::Om option);
    bool AttachBranch(const std::string &treeName, const std::string &alias, TreeOpt::Om option);
    double GetValue(const std::string &alias) const;
    void LoadEvent(Long64_t);
    Long64_t GetEntryCount();
    void WriteTrees(const std::string &outfile);

    void LoadCutConfig(const std::string &yamlPath);
    void RegisterCut(const std::string &name, const std::string &expr);
    void EnableCuts(const std::vector<std::string> &selected);
    void EnableAllCuts();
    bool PassesCut(const std::string &name) const;
    bool PassesCuts(const std::vector<std::string> &names);
    bool PassesCuts(std::initializer_list<std::string> names);
    bool PassesAllCuts();
    void WriteCutConfig(const std::string &yamlPath) const;
    std::string GetCutExpression(const std::string &name) const;
    std::vector<std::string> ListInputFiles() const;
    std::map<std::string, std::string> ListCutExpressions() const;

    void LoadHistogramConfig(const std::string &yamlPath, const std::string &prefix);
    void LoadHistogramTemplateFile(const std::string &histfile);
    void BookHistogram(const std::string &alias, std::vector<double> binfo, const std::string &prefix);
    void RegisterHistogram(const std::string &alias, TH1 *hist, const std::string &prefix);
    void FillHistograms(double weight);
    void WriteHistogramConfig(const std::string &yamlOut);
    void WriteHistograms(const std::string &outfile);

    void InitRdfFromConfig(const std::string &yamlPath);
    void InitRdfFromFile(const std::string &treename, const std::string &rootfile);

    template <typename F> void DefineRdfVariable(const std::string &name, F &&func, std::vector<std::string> vars)
    {
        if (!m_UseRdf) throw std::runtime_error("RDF not initialized");
        m_RdfNode = m_RdfNode->Define(name, std::forward<F>(func), vars);
    }
    void DefineRdfVariable(const std::string &name, const std::string &expr);

    template <typename F> void ApplyRdfFilter(const std::string &name, F &&func, std::vector<std::string> vars, const std::string &expr)
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
    void ApplyRdfFilter(const std::string &name);
    void ApplyRdfFilter(const std::string &name, const std::string &expr);
    void ApplyRdfFilters(const std::vector<std::string> &names);
    void ApplyAllRdfFilters();
    void WriteRdfSnapshot(const std::string &treeName, const std::string &fileName, TreeOpt::Om option);
    void BookRdfHistogram1D(const std::string &alias, const std::string &prefix, std::vector<double> binfo);
    void WriteRdfHistograms(const std::string &outfile);
    void BookRdfHistogramsFromConfig(const std::string &yamlPath, const std::string &prefix = "");
    void BookRdfHistogramsFromFile(const std::string &histfile);
    inline void DisableMT() { ROOT::DisableImplicitMT(); }
    LambdaManager *GetLambdaManager();
    std::ofstream OpenOutputFile(const std::string &filename, const std::string &mode = "recreate") const;

    std::unique_ptr<AnalysisManager> Fork();
    ROOT::RDF::RNode GetIsolatedRdfNode();

    inline std::vector<std::string> GetAllVarNames() { return m_RdfNode->GetColumnNames(); }
    inline std::vector<std::string> GetDefinedVarNames() { return m_RdfNode->GetDefinedColumnNames(); }

    void PrintCutSummary();
    void PrintConfigSummary();
    void PrintHistogramSummary();
    inline bool IsRdfEnabled() const { return m_UseRdf; }
    inline double GetProgress() const { return m_Progress; }

    void WriteMetadata(const std::string &filename, const std::string &hash, const std::string &baseName, const std::string &paramJson);

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
