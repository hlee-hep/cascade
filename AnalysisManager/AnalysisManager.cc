#include "AnalysisManager.hh"
#include "LambdaManager.hh"
#include <ROOT/RDFHelpers.hxx>
#include <TBranch.h>
#include <TDirectory.h>
#include <TKey.h>
#include <TLeaf.h>
#include <TROOT.h>
#include <TString.h>
#include <TSystem.h>
#include <ctime>
#include <dlfcn.h>
#include <fstream>
#include <regex>
using namespace logger;

void AnalysisManager::LoadConfig(const std::string &yamlPath)
{
    YAML::Node config = YAML::LoadFile(yamlPath);
    auto input = config["input"];
    if (input)
    {
        for (auto f : input["files"])
        {
            m_InputFiles.push_back(f.as<std::string>());
            LOG_INFO("AnalysisManager", "File " << f.as<std::string>() << " has been loaded.");
        }
        m_InTreeName = input["tree"].as<std::string>();
    }

    auto node = config["branches"];
    for (const auto &entry : node)
    {
        std::string alias = entry.first.as<std::string>();
        auto info = entry.second;
        BranchInfo binfo;
        binfo.RealName = info["name"].as<std::string>();
        if (info["type"]) binfo.Type = info["type"].as<std::string>();
        m_BranchMap[alias] = binfo;
    }
    LOG_INFO("AnalysisManager", "Configuration file for tree " << m_InTreeName << " with yaml " << yamlPath << " has been loaded.");
}

void AnalysisManager::LoadCuts(const std::string &yamlPath)
{
    YAML::Node cutsNode = YAML::LoadFile(yamlPath)["cuts"];
    for (auto it : cutsNode)
    {
        std::string name = it.first.as<std::string>();
        std::string rawExpr = it.second.as<std::string>();
        if (rawExpr.find("--lambda:") == std::string::npos) m_RawCutExpr[name] = rawExpr;
        LOG_INFO("AnalysisManager", "Registered cut '" << name << "' from " << yamlPath);
    }
    LOG_INFO("AnalysisManager", "Cut named " << yamlPath << " has been loaded.");
}

TChain *AnalysisManager::InitTree()
{

    m_CurrentTree = new TChain(m_InTreeName.c_str());
    int count = 0;
    for (auto &file : m_InputFiles)
        count += m_CurrentTree->Add(file.c_str());

    if (count < 1)
    {
        LOG_ERROR("AnalysisManager", "This initializer is for the TTree!");
        return nullptr;
    }

    for (auto &[alias, info] : m_BranchMap)
    {
        std::string realName = info.RealName;
        std::string type = info.Type;

        if (type.empty())
        {
            TLeaf *leaf = m_CurrentTree->GetLeaf(realName.c_str());
            if (!leaf)
            {
                LOG_WARN("AnalysisManager", "No leaf for " << realName << " Continue...");
                continue;
            }
            type = leaf->GetTypeName();
            info.Type = type;
        }

        if (type == "Double_t")
        {
            double *ptr = new double;
            m_CurrentTree->SetBranchAddress(realName.c_str(), ptr);
            m_BranchData[alias] = ptr;
        }
        else if (type == "Int_t")
        {
            int *ptr = new int;
            m_CurrentTree->SetBranchAddress(realName.c_str(), ptr);
            m_BranchData[alias] = ptr;
        }
        else
        {
            LOG_ERROR("AnalysisManager", "Unsupported type for " << alias << " : " << type);
        }
    }
    LOG_INFO("AnalysisManager", "Tree " << m_CurrentTree->GetName() << " is initialized.");

    return m_CurrentTree;
}

void AnalysisManager::AddTree(const std::string &name)
{
    auto it = m_TreeMap.find(name);
    if (it == m_TreeMap.end())
    {
        TTree *tree = new TTree(name.c_str(), name.c_str());
        tree->SetDirectory(nullptr);
        m_TreeMap[name] = tree;
        LOG_INFO("AnalysisManager", "Following tree is added to the lists : " << name);
    }
    else
    {
        LOG_ERROR("AnalysisManager", "Same name of tree already exist.");
    }
}

void AnalysisManager::AddTree(TTree *tree)
{
    std::string name = tree->GetName();
    auto it = m_TreeMap.find(name);
    if (it == m_TreeMap.end())
    {
        tree->SetDirectory(nullptr);
        m_TreeMap[name] = tree;
        LOG_INFO("AnalysisManager", "Following tree is added to the lists : " << name);
    }
    else
    {
        LOG_ERROR("AnalysisManager", "Same name of tree already exist.");
    }
}
void AnalysisManager::SaveTrees(const std::string &outfile)
{
    TFile *file = new TFile(outfile.c_str(), "recreate");
    file->cd();
    for (const auto &[_, tree] : m_TreeMap)
    {
        tree->Write(tree->GetName(), TObject::kOverwrite);
    }
    file->Close();
    delete file;
    LOG_INFO("AnalysisManager", "Trees are saved in " << outfile);
}
void AnalysisManager::InitNewHistsFromConfig(const std::string &yamlPath, const std::string &prefix = "")
{
    YAML::Node root = YAML::LoadFile(yamlPath);
    auto hists = root["histograms"];
    if (!hists) return;

    for (auto it : hists)
    {
        std::string alias = it.first.as<std::string>();
        std::string expr = it.second["expr"].as<std::string>();
        auto bins = it.second["bins"];

        if (bins.size() == 3)
        {
            int nbins = bins[0].as<int>();
            double xmin = bins[1].as<double>();
            double xmax = bins[2].as<double>();
            m_HistMap[alias][prefix] = {double(nbins), xmin, xmax};
            m_HistData[alias][prefix] = static_cast<TH1 *>(new TH1D(("hist_" + alias + "_" + prefix).c_str(), expr.c_str(), nbins, xmin, xmax));
        }
        else
        {
            LOG_ERROR("AnalysisManager", "Invalid bin format for histogram : " << alias);
        }
    }
}

void AnalysisManager::InitNewHistsFromFile(const std::string &histfile)
{
    LoadHists_(histfile);
    for (auto &[name, inmap] : m_LoadedHistMap)
        for (auto &[prefix, binfo] : inmap)
            m_HistMap[name][prefix] = binfo;
    for (auto &[name, inmap] : m_LoadedHistData)
    {
        for (auto &[prefix, hist] : inmap)
        {
            TString chName = hist->GetName();
            chName = chName.Remove(0, 7);
            m_HistData[name][prefix] = static_cast<TH1 *>(hist->Clone(chName));
            m_HistData[name][prefix]->Reset();
        }
    }
}
void AnalysisManager::LoadHists_(const std::string &histfile)
{
    LOG_INFO("AnalysisManager", "Loading histograms from " << histfile);
    TFile *file = new TFile(histfile.c_str());
    if (!file || file->IsZombie())
    {
        LOG_ERROR("AnalysisManager", "Failed to open histogram file: " << histfile);
        delete file;
        return;
    }

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
            TString aliasT = parsename(0, parsename.Last('_'));
            TString prefixT = parsename(parsename.Last('_') + 1, parsename.Length());
            const char *alias = aliasT.Data();
            const char *prefix = prefixT.Data();
            m_LoadedHistData[alias][prefix] = static_cast<TH1 *>(obj->Clone(Form("loaded_%s", obj->GetName())));
            auto temph = m_LoadedHistData[alias][prefix];
            double nbins = temph->GetNbinsX();
            double xmin = temph->GetBinLowEdge(1);
            double xmax = temph->GetBinLowEdge(temph->GetNbinsX()) + temph->GetBinWidth(temph->GetNbinsX());
            m_LoadedHistMap[alias][prefix] = {nbins, xmin, xmax};
            m_LoadedHistData[alias][prefix]->SetDirectory(nullptr);
        }

        delete obj;
    }

    file->Close();
    delete file;

    LOG_INFO("AnalysisManager", "Histograms are loaded from " << histfile);
}

void AnalysisManager::ActivateSelectedCuts(const std::vector<std::string> &selected = {})
{
    if (!m_CurrentTree && m_UseRdf)
    {
        LOG_ERROR("AnalysisManager", "No tree found for cuts! Please assign a "
                                     "tree first. or now using RDF");
        return;
    }
    LOG_INFO("AnalysisManager", "Activating selected cuts" << (selected.empty() ? std::string(" (all available)") : std::string("")));
    for (const auto &[name, expr] : m_RawCutExpr)
    {
        if (!selected.empty() && std::find(selected.begin(), selected.end(), name) == selected.end()) continue;
        m_CutFormulas[name] = new TTreeFormula(name.c_str(), ExpandAliases_(expr).c_str(), m_CurrentTree);
        LOG_INFO("AnalysisManager", "Cut activated: " << name << " => " << expr);
    }
}

void AnalysisManager::ActivateCuts()
{
    if (!m_CurrentTree && m_UseRdf)
    {
        LOG_ERROR("AnalysisManager", "No tree found for cuts! Please assign a tree first.");
        return;
    }
    for (const auto &[name, expr] : m_RawCutExpr)
    {
        m_CutFormulas[name] = new TTreeFormula(name.c_str(), ExpandAliases_(expr).c_str(), m_CurrentTree);
    }
    LOG_INFO("AnalysisManager", "All Cuts are activated!");
}

std::string AnalysisManager::ExpandAliases_(const std::string &expr) const
{
    std::string result = expr;
    for (const auto &[alias, binfo] : m_BranchMap)
    {
        std::regex pattern("\b" + alias + "\b");
        result = std::regex_replace(result, pattern, binfo.RealName);
    }
    return result;
}

void AnalysisManager::AddCut(const std::string &name, const std::string &expr)
{
    m_RawCutExpr[name] = expr;
    LOG_INFO("AnalysisManager", "Cut added: " << name << " -> " << expr);
}

bool AnalysisManager::PassCut(const std::string &name) const
{
    auto it = m_CutFormulas.find(name);
    return it != m_CutFormulas.end() && it->second->EvalInstance();
}

bool AnalysisManager::PassCut(const std::vector<std::string> &names)
{
    for (const auto &name : names)
        if (!PassCut(name)) return false;
    return true;
}

bool AnalysisManager::PassCut(std::initializer_list<std::string> names)
{
    for (const auto &name : names)
        if (!PassCut(name)) return false;
    return true;
}

bool AnalysisManager::PassCut()
{
    for (const auto &[name, _] : m_CutFormulas)
        if (!PassCut(name)) return false;
    return true;
}

double AnalysisManager::GetVar(const std::string &alias) const
{
    auto it = m_BranchData.find(alias);
    if (it == m_BranchData.end())
    {
        return GetNewVar_(alias);
    }
    else
    {
        auto type = m_BranchMap.at(alias).Type;
        if (type == "Double_t") return *(double *)m_BranchData.at(alias);
        if (type == "Int_t") return *(int *)m_BranchData.at(alias);
        LOG_ERROR("AnalysisManager", "Unsupported type in GetVar");
        return -255;
    }
}

double AnalysisManager::GetNewVar_(const std::string &alias) const
{
    auto it = m_NewBranchData.find(alias);
    if (it == m_NewBranchData.end())
    {
        LOG_ERROR("AnalysisManager", "Cannot find " << alias << " in the variables list!");
        return -255;
    }
    else
    {
        return *m_NewBranchData.at(alias);
    }
}

std::string AnalysisManager::GetCutString(const std::string &name) const
{
    auto it = m_RawCutExpr.find(name);
    if (it != m_RawCutExpr.end())
        return it->second;
    else
    {
        LOG_ERROR("AnalysisManager", "Cannot find " << name << " in the list!");
        return std::string("");
    }
}

void AnalysisManager::SaveCuts(const std::string &yamlPath) const
{
    YAML::Emitter out;
    out << YAML::BeginMap;
    out << YAML::Key << "cuts" << YAML::Value << YAML::BeginMap;
    for (const auto &[name, expr] : m_RawCutExpr)
    {
        out << YAML::Key << name << YAML::Value << expr;
    }
    out << YAML::EndMap << YAML::EndMap;
    std::ofstream fout(yamlPath);
    if (!fout)
    {
        LOG_ERROR("AnalysisManager", "Unable to open cut output file: " << yamlPath);
        return;
    }

    fout << out.c_str();
    LOG_INFO("AnalysisManager", "Cuts are saved to " << yamlPath);
}

void AnalysisManager::GenerateConfig(TTree *tree, const std::string &yamlOut, const std::vector<std::string> &filenames)
{
    LOG_INFO("AnalysisManager", "Generating configuration for tree " << tree->GetName() << " to " << yamlOut);
    YAML::Emitter out;
    out << YAML::BeginMap;

    out << YAML::Key << "input" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "files" << YAML::Value << YAML::BeginSeq;
    for (const auto &f : filenames)
        out << f;
    out << YAML::EndSeq;
    out << YAML::Key << "tree" << YAML::Value << tree->GetName();
    out << YAML::EndMap;

    out << YAML::Key << "branches" << YAML::Value << YAML::BeginMap;
    auto *blist = tree->GetListOfBranches();
    for (int i = 0; i < blist->GetEntries(); ++i)
    {
        TBranch *b = (TBranch *)blist->At(i);
        std::string name = b->GetName();
        std::string type = b->GetLeaf(name.c_str())->GetTypeName();

        out << YAML::Key << name << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "name" << YAML::Value << name;
        // out << YAML::Key << "type" << YAML::Value << type;
        out << YAML::EndMap;
    }
    out << YAML::EndMap;

    out << YAML::EndMap;

    std::ofstream fout(yamlOut);
    if (!fout)
    {
        LOG_ERROR("AnalysisManager", "Unable to write generated config to " << yamlOut);
        return;
    }

    fout << out.c_str();
    fout.close();
    LOG_INFO("AnalysisManager", "Configuration is generated at " << yamlOut);
}

double *AnalysisManager::AddVar(const std::string &name, std::string alias = "")
{
    if (alias.length() == 0) alias = name;

    auto it = m_NewBranchMap.find(alias);
    if (it != m_NewBranchMap.end())
    {
        LOG_ERROR("AnalysisManager", "Overwriting existing new variable : " << alias);
        return nullptr;
    }

    double *ptr = new double;
    m_NewBranchMap[alias] = {name, "Double_t"};
    m_NewBranchData[alias] = ptr;
    LOG_INFO("AnalysisManager", "New variable added: " << alias << " mapped to " << name);
    return ptr;
}

bool AnalysisManager::AddBranch(TTree *tree, const std::string &alias, TreeOpt::Om option)
{
    if (tree == m_CurrentTree)
    {
        LOG_ERROR("AnalysisManager", "Cannot overwrite current tree!");
        return false;
    }
    else
    {
        auto it = m_NewBranchMap.find(alias);
        if (it == m_NewBranchMap.end())
        {
            LOG_ERROR("AnalysisManager", "No such variable in the new branch map : " << alias);
            return false;
        }
        double *ptr = m_NewBranchData[alias];
        std::string name = m_NewBranchMap[alias].RealName;
        auto itold = m_BranchMap.find(alias);
        if (option == TreeOpt::Om::Append)
        {
            if (itold == m_BranchMap.end())
            {
                LOG_INFO("AnalysisManager", "New branch named " << name << " for " << tree->GetName() << " is appended.");
                tree->Branch(name.c_str(), ptr, (name + "/D").c_str());
                return true;
            }
            else
            {
                LOG_INFO("AnalysisManager", "Cannot overload existing branch : " << alias);
                return false;
            }
        }
        else if (option == TreeOpt::Om::Recreate)
        {
            LOG_INFO("AnalysisManager", "New branch named " << name << " for " << tree->GetName() << "is created.");
            tree->Branch(name.c_str(), ptr, (name + "/D").c_str());
            return true;
        }
        else
        {
            LOG_ERROR("AnalysisManager", "Option should be Append or Recreate!");
            return false;
        }
    }
}

bool AnalysisManager::AddBranch(const std::string &treeName, const std::string &alias, TreeOpt::Om option)
{
    auto itTree = m_TreeMap.find(treeName);
    if (itTree == m_TreeMap.end())
    {
        LOG_ERROR("AnalysisManager", "Cannot find the tree!");
        return false;
    }
    else
    {
        auto it = m_NewBranchMap.find(alias);
        if (it == m_NewBranchMap.end())
        {
            LOG_ERROR("AnalysisManager", "No such variable in the new branch map : " << alias);
            return false;
        }
        double *ptr = m_NewBranchData[alias];
        std::string name = m_NewBranchMap[alias].RealName;
        auto itold = m_BranchMap.find(alias);
        if (option == TreeOpt::Om::Append)
        {
            if (itold == m_BranchMap.end())
            {
                LOG_INFO("AnalysisManager", "New branch named " << name << " for " << treeName << " is appended.");
                itTree->second->Branch(name.c_str(), ptr, (name + "/D").c_str());
                return true;
            }
            else
            {
                LOG_INFO("AnalysisManager", "Cannot overload existing branch : " << alias);
                return false;
            }
        }
        else if (option == TreeOpt::Om::Recreate)
        {
            LOG_INFO("AnalysisManager", "New branch named " << name << " for " << treeName << " is created.");
            itTree->second->Branch(name.c_str(), ptr, (name + "/D").c_str());
            return true;
        }
        else
        {
            LOG_ERROR("AnalysisManager", "Option should be Append or Recreate!");
            return false;
        }
    }
}
void AnalysisManager::AddHist(const std::string &alias, std::vector<double> binfo, const std::string &prefix = "")
{
    std::string fullname = "hist_" + alias + "_" + prefix;
    bool chk = true;
    for (const auto &[alias, inmap] : m_HistMap)
    {
        std::string existname = "hist_" + alias;
        for (const auto &[prefix, _] : inmap)
        {
            existname += "_" + prefix;
            if (existname == fullname)
            {
                chk = false;
            }
            if (!chk) break;
        }
        if (!chk) break;
    }
    if (chk)
    {
        LOG_INFO("AnalysisManager", "Histogram " << fullname << " is added");
        m_HistData[alias][prefix] = static_cast<TH1 *>(new TH1D(fullname.c_str(), "", int(binfo[0]), binfo[1], binfo[2]));
        m_HistData[alias][prefix]->SetDirectory(nullptr);
        m_HistMap[alias][prefix] = binfo;
    }
    else
    {
        LOG_ERROR("AnalysisManager", "The same histogram exists. name : " << fullname);
    }
}

void AnalysisManager::AddHist(const std::string &alias, TH1 *hist, const std::string &prefix = "")
{
    std::string fullname = hist->GetName();
    bool chk = true;
    for (const auto &[alias, inmap] : m_HistMap)
    {
        std::string existname = "hist_" + alias;
        for (const auto &[prefix, _] : inmap)
        {
            existname += "_" + prefix;
            if (existname == fullname)
            {
                chk = false;
            }
            if (!chk) break;
        }
        if (!chk) break;
    }
    if (chk)
    {
        LOG_INFO("AnalysisManager", "Histogram " << fullname << " is added");
        m_HistData[alias][prefix] = hist;
        m_HistData[alias][prefix]->SetDirectory(nullptr);
        double nbins = hist->GetNbinsX();
        double xmin = hist->GetBinLowEdge(1);
        double xmax = hist->GetBinLowEdge(hist->GetNbinsX()) + hist->GetBinWidth(hist->GetNbinsX());
        m_HistMap[alias][prefix] = {nbins, xmin, xmax};
    }
    else
    {
        LOG_ERROR("AnalysisManager", "The same histogram exists. : " << fullname);
    }
}

void AnalysisManager::FillHists(double weight)
{
    for (const auto &[name, inmap] : m_HistData)
    {
        double x = GetVar(name);

        for (const auto &[_, hist] : inmap)
            hist->Fill(x, weight);
    }
}

void AnalysisManager::SaveHists(const std::string &outfile)
{
    TFile *file = new TFile(outfile.c_str(), "recreate");
    file->cd();
    for (const auto &[_, inmap] : m_HistData)
    {
        for (const auto &[_, hist] : inmap)
            hist->Write(hist->GetName(), TObject::kOverwrite);
    }
    file->Close();
    delete file;
    LOG_INFO("AnalysisManager", "Histograms are saved in " << outfile);
}

void AnalysisManager::SaveHistsConfig(const std::string &yamlOut)
{
    YAML::Emitter out;
    out << YAML::BeginMap;

    out << YAML::Key << "histograms" << YAML::Value << YAML::BeginMap;

    for (const auto &[name, inmap] : m_HistMap)
    {
        out << YAML::Key << name << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "expr" << YAML::Value << name;
        auto firstIt = inmap.begin();
        out << YAML::Key << "bins" << YAML::Value << YAML::Flow << YAML::BeginSeq;
        for (auto b : firstIt->second)
            out << b;
        out << YAML::EndSeq;
        out << YAML::EndMap;
    }
    out << YAML::EndMap;

    out << YAML::EndMap;

    std::ofstream fout(yamlOut);
    fout << out.c_str();
    fout.close();
}

void AnalysisManager::LoadEntry(Long64_t i)
{
    if (i == 0) m_StartTime = std::chrono::steady_clock::now();

    m_CurrentTree->GetEntry(i);

    if (i % 500 == 0 || i == GetEntries() - 1) UpdateProgress_((double)(i + 1) / GetEntries());
}

Long64_t AnalysisManager::GetEntries() { return m_CurrentTree->GetEntries(); }

void AnalysisManager::SetRDFInputFromConfig(const std::string &yamlPath)
{
    YAML::Node config = YAML::LoadFile(yamlPath);
    auto input = config["input"];
    if (input)
    {
        for (auto f : input["files"])
        {
            m_InputFiles.push_back(f.as<std::string>());
            LOG_INFO("AnalysisManager", "File " << f.as<std::string>() << " has been loaded.");
        }
        m_InTreeName = input["tree"].as<std::string>();

        m_CurrentTree = new TChain(m_InTreeName.c_str());
        int count = 0;
        for (auto &file : m_InputFiles)
            count += m_CurrentTree->Add(file.c_str());

        if (count < 1)
        {
            LOG_ERROR("AnalysisManager", "This initializer is for the TTree!");
            return;
        }
        m_UseRdf = true;
        m_RdfRaw = std::make_unique<ROOT::RDataFrame>(*m_CurrentTree);
        m_RdfNode = *m_RdfRaw;
        m_LambdaManager = std::make_unique<LambdaManager>();
        LOG_INFO("AnalysisManager", "RDF input initialized from config " << yamlPath << " with " << m_InputFiles.size()
                                      << " files for tree " << m_InTreeName);
    }
    else
    {
        LOG_ERROR("AnalysisManager", "Cannot find input!");
        return;
    }
}

void AnalysisManager::SetRDFInputFromFile(const std::string &treename, const std::string &filename)
{
    m_CurrentTree = new TChain(treename.c_str());
    m_CurrentTree->Add(filename.c_str());
    m_UseRdf = true;
    m_RdfRaw = std::make_unique<ROOT::RDataFrame>(*m_CurrentTree);
    m_RdfNode = *m_RdfRaw;
    m_LambdaManager = std::make_unique<LambdaManager>();
    LOG_INFO("AnalysisManager", "RDF input initialized from file " << filename << " for tree " << treename);
}

void AnalysisManager::DefineRDFVar(const std::string &name, const std::string &expr)
{
    if (!m_UseRdf) throw std::runtime_error("RDF not initialized");
    m_RdfNode = m_RdfNode->Define(name, expr);
    LOG_INFO("AnalysisManager", "Defined RDF variable '" << name << "' with expression '" << expr << "'");
}

void AnalysisManager::ApplyRDFFilter(const std::string &name)
{
    if (!m_UseRdf) throw std::runtime_error("RDF not initialized");

    auto it = m_RawCutExpr.find(name);
    if (it != m_RawCutExpr.end())
    {
        if (it->second.find("--lambda:") == std::string::npos)
        {
            LOG_INFO("AnalysisManager", "Cut " << name << " : " << it->second << " is applied.");
            m_RdfNode = m_RdfNode->Filter(it->second, name);
        }
    }
    else
        LOG_ERROR("AnalysisManager", "Cannot find cut '" << name << "', filter is not applied.");
}

void AnalysisManager::ApplyRDFFilter(const std::string &name, const std::string &expr)
{
    if (!m_UseRdf) throw std::runtime_error("RDF not initialized");
    auto it = m_RawCutExpr.find(name);
    if (it != m_RawCutExpr.end())
    {
        LOG_ERROR("AnalysisManager", "SAME name of cut exists. expr : " << it->second);
    }
    else
    {
        m_RdfNode = m_RdfNode->Filter(expr, name);
        LOG_INFO("AnalysisManager", "Direct filter is applied and registered. name : " << name << " and expr : " << expr);
        m_RawCutExpr[name] = expr;
    }
}

void AnalysisManager::ApplyRDFFilterSelected(const std::vector<std::string> &names)
{
    LOG_INFO("AnalysisManager", "Applying RDF filters for selected cuts (" << names.size() << " entries)");
    for (const auto &n : names)
        ApplyRDFFilter(n);
}
void AnalysisManager::ApplyRDFFilterAll()
{
    LOG_INFO("AnalysisManager", "Applying RDF filters for all registered cuts");
    for (const auto &[name, _] : m_RawCutExpr)
        ApplyRDFFilter(name);
}
void AnalysisManager::BookRDFHist1D(const std::string &alias, const std::string &prefix, std::vector<double> binfo)
{
    if (!m_RdfNode) throw std::runtime_error("RDF not initialized!");
    if (binfo.size() != 3)
    {
        LOG_ERROR("AnalysisManager", "bin info should has form such as {100,0,1}");
        return;
    }
    std::string fullname = "hist_" + alias + "_" + prefix;
    auto model = ROOT::RDF::TH1DModel(fullname.c_str(), "", int(binfo[0]), binfo[1], binfo[2]);
    auto rptr = m_RdfNode->Histo1D(model, alias);
    m_HistRdf[alias][prefix] = rptr;
    LOG_INFO("AnalysisManager", "Booked RDF histogram '" << fullname << "' with bins {" << binfo[0] << ", " << binfo[1] << ", "
                                           << binfo[2] << "}");
}

void AnalysisManager::BookRDFHistsFromConfig(const std::string &yamlPath, const std::string &prefix)
{
    YAML::Node root = YAML::LoadFile(yamlPath);
    auto hists = root["histograms"];
    if (!hists) return;
    for (auto it : hists)
    {
        std::string alias = it.first.as<std::string>();
        std::string expr = it.second["expr"].as<std::string>();
        auto bins = it.second["bins"];

        if (bins.size() == 3)
        {
            int nbins = bins[0].as<int>();
            double xmin = bins[1].as<double>();
            double xmax = bins[2].as<double>();
            BookRDFHist1D(alias, prefix, {double(nbins), xmin, xmax});
        }
        else
        {
            LOG_ERROR("AnalysisManager", "Invalid bin format for histogram : " << alias);
        }
    }
    LOG_INFO("AnalysisManager", "Booked RDF histograms from config " << yamlPath << " with prefix '" << prefix << "'");
}
void AnalysisManager::BookRDFHistsFromFile(const std::string &histfile)
{
    LoadHists_(histfile);
    for (auto &[alias, inmap] : m_LoadedHistMap)
    {
        for (auto &[prefix, binfo] : inmap)
        {
            BookRDFHist1D(alias, prefix, binfo);
        }
    }
    LOG_INFO("AnalysisManager", "Booked RDF histograms based on file " << histfile);
}

void AnalysisManager::SnapshotRDF(const std::string &treeName, const std::string &fileName, TreeOpt::Om option)
{
    if (!m_UseRdf) throw std::runtime_error("RDF not initialized");
    std::atomic<ULong64_t> counter = 0;
    auto callback = m_RdfNode->Count();

    callback.OnPartialResultSlot(500,
                                 [this, &counter](unsigned int j, ULong64_t &)
                                 {
                                     std::lock_guard<std::mutex> lock(m_ProgressMutex);
                                     counter += 500;
                                     UpdateProgress_(double(counter) / GetEntries());
                                 });
    // ROOT::RDF::Experimental::AddProgressBar(*m_RdfNode);
    ROOT::RDF::RSnapshotOptions opts;
    opts.fLazy = false;
    if (option == TreeOpt::Om::Recreate)
        m_RdfNode->Snapshot(treeName, fileName, GetDefinedVarNames(), opts);
    else if (option == TreeOpt::Om::Append)
        m_RdfNode->Snapshot(treeName, fileName, GetAllVarNames(), opts);
    else
        throw std::runtime_error("No such option avaliable");
    m_StartTime = std::chrono::steady_clock::now();
    *callback;
    UpdateProgress_(1.0);
}

void AnalysisManager::SaveHistsRDF(const std::string &outfile)
{
    if (!m_UseRdf) throw std::runtime_error("RDF not initialized");

    std::atomic<ULong64_t> counter = 0;
    auto callback = m_RdfNode->Count();

    callback.OnPartialResultSlot(500,
                                 [this, &counter](unsigned int, ULong64_t &)
                                 {
                                     std::lock_guard<std::mutex> lock(m_ProgressMutex);
                                     counter += 500;
                                     UpdateProgress_(double(counter) / GetEntries());
                                 });
    // ROOT::RDF::Experimental::AddProgressBar(*m_RdfNode);
    TFile *file = new TFile(outfile.c_str(), "recreate");
    file->cd();

    m_StartTime = std::chrono::steady_clock::now();
    *callback;
    for (auto &[_, inmap] : m_HistRdf)
    {
        for (auto &[_, hist] : inmap)
            hist->Write(hist->GetName(), TObject::kOverwrite);
    }
    file->Close();
    delete file;
    UpdateProgress_(1.0);
    LOG_INFO("AnalysisManager", "RDF Histograms are saved in " << outfile);
}

std::unique_ptr<AnalysisManager> AnalysisManager::Fork()
{
    auto forked = std::make_unique<AnalysisManager>();
    forked->m_RdfNode = this->m_RdfNode->Filter([]() { return true; });
    forked->m_UseRdf = true;
    forked->m_RawCutExpr = this->m_RawCutExpr;
    forked->m_LambdaManager = std::make_unique<LambdaManager>();

    return forked;
}

ROOT::RDF::RNode AnalysisManager::GetIsolatedRNode()
{
    if (!m_RdfNode.has_value()) throw std::runtime_error("m_RdfNode not initialized");

    LOG_INFO("AnalysisManager", "Isolated RNode is generated. Manager has no responsible for the RNode.");
    return m_RdfNode->Filter([]() { return true; });
}

LambdaManager *AnalysisManager::GetLambdaManager()
{
    if (!m_UseRdf) throw std::runtime_error("RDF not initialized");
    return m_LambdaManager.get();
}

std::ofstream AnalysisManager::OpenResultFile(const std::string &filename, const std::string &mode) const
{
    std::ios_base::openmode openMode = std::ios::out;
    TString tmode = mode;

    if (tmode.Contains("update", TString::kIgnoreCase))
    {
        LOG_INFO("AnalysisManager", filename << " is opened with update mode.");
        openMode |= std::ios::app;
    }
    else
    {
        LOG_INFO("AnalysisManager", filename << " is opened.");
        openMode |= std::ios::trunc;
    }

    std::ofstream fout(filename, openMode);
    if (!fout) throw std::runtime_error("Failed to open result file : " + filename);
    return fout;
}

void AnalysisManager::UpdateProgress_(double p)
{
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - m_StartTime).count();
    double eta = (p > 0.0f && p < 1.0f) ? elapsed * (1.0f / p - 1.0f) : -1.0f;
    m_Progress = std::clamp(p, 0.0, 1.0);
    Logger::Get().PrintProgressBar("AnalysisManager", m_Progress, elapsed, eta);
}

std::vector<std::string> AnalysisManager::GetInputFiles() const { return m_InputFiles; }

std::map<std::string, std::string> AnalysisManager::GetCutExpressions() const { return m_RawCutExpr; }

void AnalysisManager::WriteMetaData(const std::string &filename, const std::string &hash, const std::string &baseName, const std::string &paramJson)
{
    m_Hash = hash;
    m_Basename = baseName;
    m_ParamJson = paramJson;
    m_Cuts = "";
    m_EndTime = Logger::Get().GetCurrentTime();
    for (const auto &[k, v] : m_RawCutExpr)
        m_Cuts += (k + ":" + v + ";");
    TFile fout(filename.c_str(), "UPDATE");
    TTree *tmeta = new TTree("metadata", "metadata");
    tmeta->Branch("hash", &m_Hash);
    tmeta->Branch("endtime", &m_EndTime);
    tmeta->Branch("module", &m_Basename);
    tmeta->Branch("paramjson", &m_ParamJson);
    tmeta->Branch("cuts", &m_Cuts);
    tmeta->Fill();
    tmeta->Write("metadata");
    fout.Close();
}
///////////////////////////////////////////////////////////////////////////////////////
void AnalysisManager::PrintCuts()
{
    LOG_INFO("AnalysisManager", "-------------REGISTERED CUTS----------------");
    for (const auto &[name, expr] : m_RawCutExpr)
        LOG_INFO("AnalysisManager", name << " : " << expr);
    LOG_INFO("AnalysisManager", "-------------REGISTERED CUTS----------------");
}

void AnalysisManager::PrintConfig()
{
    LOG_INFO("AnalysisManager", "-------------Configuration Summary------------");
    LOG_INFO("AnalysisManager", "FILES :");
    for (const auto &filename : m_InputFiles)
        LOG_INFO("AnalysisManager", "    " << filename);
    LOG_INFO("AnalysisManager", "    TREE : " << m_InTreeName);
    if (!m_UseRdf)
    {
        LOG_INFO("AnalysisManager", "        BRANCHES (ALIAS : BRANCH NAME / TYPE): ");
        for (const auto &[alias, binfo] : m_BranchMap)
        {
            LOG_INFO("AnalysisManager", "               " << alias << " : " << binfo.RealName << " / " << binfo.Type);
        }
    }
    LOG_INFO("AnalysisManager", "-------------Configuration Summary-------------");
}

void AnalysisManager::PrintHists()
{
    LOG_INFO("AnalysisManager", "-------------Registered Histograms-------------");
    for (const auto &[alias, hists] : m_LoadedHistMap)
    {
        for (const auto &[prefix, bins] : hists)
            LOG_INFO("AnalysisManager", "loaded_hist_" << alias << "_" << prefix << " : " << "[" << int(bins[0]) << "," << bins[1] << "," << bins[2] << "]");
    }

    for (const auto &[alias, hists] : m_HistMap)
    {
        for (const auto &[prefix, bins] : hists)
            LOG_INFO("AnalysisManager", "hist_" << alias << "_" << prefix << " : " << "[" << int(bins[0]) << "," << bins[1] << "," << bins[2] << "]");
    }
    if (m_UseRdf)
    {
        for (const auto &[alias, hists] : m_HistRdf)
            for (const auto &[prefix, _] : hists)
                LOG_INFO("AnalysisManager", "hist_" << alias << "_" << prefix << " : RDF histograms");
    }
    LOG_INFO("AnalysisManager", "-------------Registered Histograms-------------");
}

AnalysisManager::AnalysisManager()
{
    void *handle = dlopen("libTreePlayer.so", RTLD_LAZY | RTLD_GLOBAL);
    if (!handle)
    {
        LOG_WARN("AnalysisManager", "Failed to load libTreePlayer.so: " << dlerror());
    }
    ROOT::EnableImplicitMT();
}

AnalysisManager::~AnalysisManager()
{
    for (auto &[_, tr] : m_TreeMap)
        delete tr;
    m_TreeMap.clear();
    for (auto &[alias, ptr] : m_BranchData)
    {
        if (m_BranchMap[alias].Type == "Int_t")
            delete static_cast<int *>(ptr);
        else
            delete static_cast<double *>(ptr);
    }
    m_BranchData.clear();
    m_BranchMap.clear();
    m_NewBranchMap.clear();
    for (auto &[_, ptr] : m_NewBranchData)
        delete ptr;
    m_NewBranchData.clear();
    m_HistMap.clear();
    for (auto &[_, hists] : m_HistData)
        for (auto &[_, h] : hists)
            delete h;
    m_HistData.clear();
    m_LoadedHistMap.clear();
    for (auto &[_, hists] : m_LoadedHistData)
        for (auto &[_, h] : hists)
            delete h;
    m_LoadedHistData.clear();
    m_HistRdf.clear();
    m_RawCutExpr.clear();
    for (auto &[_, cut] : m_CutFormulas)
        delete cut;
    m_CutFormulas.clear();
    m_InputFiles.clear();
    delete m_CurrentTree;
}
