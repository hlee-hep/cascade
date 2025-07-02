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
#include <iostream>
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
            inputFiles.push_back(f.as<std::string>());
            LOG_INFO("AnalysisManager",
                     "File " << f.as<std::string>() << " has been loaded.");
        }
        inTreeName = input["tree"].as<std::string>();
    }

    auto node = config["branches"];
    for (const auto &entry : node)
    {
        std::string alias = entry.first.as<std::string>();
        auto info = entry.second;
        BranchInfo binfo;
        binfo.realName = info["name"].as<std::string>();
        if (info["type"])
            binfo.type = info["type"].as<std::string>();
        branchMap[alias] = binfo;
    }
    LOG_INFO("AnalysisManager", "Configuration file for tree "
                                    << inTreeName << " with yaml " << yamlPath
                                    << " has been loaded.");
}

void AnalysisManager::LoadCuts(const std::string &yamlPath)
{
    YAML::Node cutsNode = YAML::LoadFile(yamlPath)["cuts"];
    for (auto it : cutsNode)
    {
        std::string name = it.first.as<std::string>();
        std::string rawExpr = it.second.as<std::string>();
        if (rawExpr.find("--lambda:") == std::string::npos)
            rawCutExpr[name] = rawExpr;
    }
    LOG_INFO("AnalysisManager",
             "Cut named " << yamlPath << " has been loaded.");
}

TChain *AnalysisManager::InitTree()
{

    currentTree = new TChain(inTreeName.c_str());
    int count = 0;
    for (auto &file : inputFiles)
        count += currentTree->Add(file.c_str());

    if (count < 1)
    {
        LOG_ERROR("AnalysisManager", "This initializer is for the TTree!");
        return nullptr;
    }

    for (auto &[alias, info] : branchMap)
    {
        std::string realName = info.realName;
        std::string type = info.type;

        if (type.empty())
        {
            TLeaf *leaf = currentTree->GetLeaf(realName.c_str());
            if (!leaf)
            {
                LOG_WARN("AnalysisManager",
                         "No leaf for " << realName << " Continue...");
                continue;
            }
            type = leaf->GetTypeName();
            info.type = type;
        }

        if (type == "Double_t")
        {
            double *ptr = new double;
            currentTree->SetBranchAddress(realName.c_str(), ptr);
            branchData[alias] = ptr;
        }
        else if (type == "Int_t")
        {
            int *ptr = new int;
            currentTree->SetBranchAddress(realName.c_str(), ptr);
            branchData[alias] = ptr;
        }
        else
        {
            LOG_ERROR("AnalysisManager",
                      "Unsupported type for " << alias << " : " << type);
        }
    }
    LOG_INFO("AnalysisManager",
             "Tree " << currentTree->GetName() << " is initialized.");

    return currentTree;
}

void AnalysisManager::AddTree(const std::string &name)
{
    auto it = treeMap.find(name);
    if (it == treeMap.end())
    {
        TTree *tree = new TTree(name.c_str(), name.c_str());
        tree->SetDirectory(nullptr);
        treeMap[name] = tree;
        LOG_INFO("AnalysisManager",
                 "Following tree is added to the lists : " << name);
    }
    else
    {
        LOG_ERROR("AnalysisManager", "Same name of tree already exist.");
    }
}

void AnalysisManager::AddTree(TTree *tree)
{
    std::string name = tree->GetName();
    auto it = treeMap.find(name);
    if (it == treeMap.end())
    {
        tree->SetDirectory(nullptr);
        treeMap[name] = tree;
        LOG_INFO("AnalysisManager",
                 "Following tree is added to the lists : " << name);
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
    for (const auto &[_, tree] : treeMap)
    {
        tree->Write(tree->GetName(), TObject::kOverwrite);
    }
    file->Close();
    delete file;
    LOG_INFO("AnalysisManager", "Trees are saved in " << outfile);
}
void AnalysisManager::InitNewHistsFromConfig(const std::string &yamlPath,
                                             const std::string &prefix = "")
{
    YAML::Node root = YAML::LoadFile(yamlPath);
    auto hists = root["histograms"];
    if (!hists)
        return;

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
            histMap[alias][prefix] = {double(nbins), xmin, xmax};
            histData[alias][prefix] = static_cast<TH1 *>(
                new TH1D(("hist_" + alias + "_" + prefix).c_str(), expr.c_str(),
                         nbins, xmin, xmax));
        }
        else
        {
            LOG_ERROR("AnalysisManager",
                      "Invalid bin format for histogram : " << alias);
        }
    }
}

void AnalysisManager::InitNewHistsFromFile(const std::string &histfile)
{
    LoadHists(histfile);
    for (auto &[name, inmap] : loadedHistMap)
        for (auto &[prefix, binfo] : inmap)
            histMap[name][prefix] = binfo;
    for (auto &[name, inmap] : loadedHistData)
    {
        for (auto &[prefix, hist] : inmap)
        {
            TString ch_name = hist->GetName();
            ch_name = ch_name.Remove(0, 7);
            histData[name][prefix] = static_cast<TH1 *>(hist->Clone(ch_name));
            histData[name][prefix]->Reset();
        }
    }
}
void AnalysisManager::LoadHists(const std::string &histfile)
{
    TFile *file = new TFile(histfile.c_str());
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
            loadedHistData[alias][prefix] = static_cast<TH1 *>(
                obj->Clone(Form("loaded_%s", obj->GetName())));
            auto temph = loadedHistData[alias][prefix];
            double nbins = temph->GetNbinsX();
            double xmin = temph->GetBinLowEdge(1);
            double xmax = temph->GetBinLowEdge(temph->GetNbinsX()) +
                          temph->GetBinWidth(temph->GetNbinsX());
            loadedHistMap[alias][prefix] = {nbins, xmin, xmax};
            loadedHistData[alias][prefix]->SetDirectory(nullptr);
        }

        delete obj;
    }

    file->Close();
    delete file;

    LOG_INFO("AnalysisManager", "Histograms are loaded from " << histfile);
}

void AnalysisManager::ActivateSelectedCuts(
    const std::vector<std::string> &selected = {})
{
    if (!currentTree && useRDF)
    {
        LOG_ERROR("AnalysisManager", "No tree found for cuts! Please assign a "
                                     "tree first. or now using RDF");
        return;
    }
    for (const auto &[name, expr] : rawCutExpr)
    {
        if (!selected.empty() &&
            std::find(selected.begin(), selected.end(), name) == selected.end())
            continue;
        cutFormulas[name] = new TTreeFormula(
            name.c_str(), ExpandAliases(expr).c_str(), currentTree);
    }
}

void AnalysisManager::ActivateCuts()
{
    if (!currentTree && useRDF)
    {
        LOG_ERROR("AnalysisManager",
                  "No tree found for cuts! Please assign a tree first.");
        return;
    }
    for (const auto &[name, expr] : rawCutExpr)
    {
        cutFormulas[name] = new TTreeFormula(
            name.c_str(), ExpandAliases(expr).c_str(), currentTree);
    }
    LOG_INFO("AnalysisManager", "All Cuts are activated!");
}

std::string AnalysisManager::ExpandAliases(const std::string &expr) const
{
    std::string result = expr;
    for (const auto &[alias, binfo] : branchMap)
    {
        std::regex pattern("\b" + alias + "\b");
        result = std::regex_replace(result, pattern, binfo.realName);
    }
    return result;
}

void AnalysisManager::AddCut(const std::string &name, const std::string &expr)
{
    rawCutExpr[name] = expr;
}

bool AnalysisManager::PassCut(const std::string &name) const
{
    auto it = cutFormulas.find(name);
    return it != cutFormulas.end() && it->second->EvalInstance();
}

bool AnalysisManager::PassCut(const std::vector<std::string> &names)
{
    for (const auto &name : names)
        if (!PassCut(name))
            return false;
    return true;
}

bool AnalysisManager::PassCut(std::initializer_list<std::string> names)
{
    for (const auto &name : names)
        if (!PassCut(name))
            return false;
    return true;
}

bool AnalysisManager::PassCut()
{
    for (const auto &[name, _] : cutFormulas)
        if (!PassCut(name))
            return false;
    return true;
}

double AnalysisManager::GetVar(const std::string &alias) const
{
    auto it = branchData.find(alias);
    if (it == branchData.end())
    {
        return GetNewVar(alias);
    }
    else
    {
        auto type = branchMap.at(alias).type;
        if (type == "Double_t")
            return *(double *)branchData.at(alias);
        if (type == "Int_t")
            return *(int *)branchData.at(alias);
        LOG_ERROR("AnalysisManager", "Unsupported type in GetVar");
        return -255;
    }
}

double AnalysisManager::GetNewVar(const std::string &alias) const
{
    auto it = newBranchData.find(alias);
    if (it == newBranchData.end())
    {
        LOG_ERROR("AnalysisManager",
                  "Cannot find " << alias << " in the variables list!");
        return -255;
    }
    else
    {
        return *newBranchData.at(alias);
    }
}

std::string AnalysisManager::GetCutString(const std::string &name) const
{
    auto it = rawCutExpr.find(name);
    if (it != rawCutExpr.end())
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
    for (const auto &[name, expr] : rawCutExpr)
    {
        out << YAML::Key << name << YAML::Value << expr;
    }
    out << YAML::EndMap << YAML::EndMap;
    std::ofstream fout(yamlPath);
    fout << out.c_str();
}

void AnalysisManager::GenerateConfig(TTree *tree, const std::string &yamlOut,
                                     const std::vector<std::string> &filenames)
{
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
    fout << out.c_str();
    fout.close();
}

double *AnalysisManager::AddVar(const std::string &name, std::string alias = "")
{
    if (alias.length() == 0)
        alias = name;

    auto it = newBranchMap.find(alias);
    if (it != newBranchMap.end())
    {
        LOG_ERROR("AnalysisManager",
                  "Overwriting existing new variable : " << alias);
        return nullptr;
    }

    double *ptr = new double;
    newBranchMap[alias] = {name, "Double_t"};
    newBranchData[alias] = ptr;
    return ptr;
}

bool AnalysisManager::AddBranch(TTree *tree, const std::string &alias,
                                int option)
{
    if (tree == currentTree)
    {
        LOG_ERROR("AnalysisManager", "Cannot overwrite current tree!");
        return false;
    }
    else
    {
        auto it = newBranchMap.find(alias);
        if (it == newBranchMap.end())
        {
            LOG_ERROR("AnalysisManager",
                      "No such variable in the new branch map : " << alias);
            return false;
        }
        double *ptr = newBranchData[alias];
        std::string name = newBranchMap[alias].realName;
        auto itold = branchMap.find(alias);
        if (option == TreeOpt::OM::kAppend)
        {
            if (itold == branchMap.end())
            {
                LOG_INFO("AnalysisManager", "New branch named "
                                                << name << " for "
                                                << tree->GetName()
                                                << " is appended.");
                tree->Branch(name.c_str(), ptr, (name + "/D").c_str());
                return true;
            }
            else
            {
                LOG_INFO("AnalysisManager",
                         "Cannot overload existing branch : " << alias);
                return false;
            }
        }
        else if (option == TreeOpt::OM::kRecreate)
        {
            LOG_INFO("AnalysisManager", "New branch named " << name << " for "
                                                            << tree->GetName()
                                                            << "is created.");
            tree->Branch(name.c_str(), ptr, (name + "/D").c_str());
            return true;
        }
        else
        {
            LOG_ERROR("AnalysisManager",
                      "Option should be kAppend or kRecreate!");
            return false;
        }
    }
}

bool AnalysisManager::AddBranch(const std::string &treeName,
                                const std::string &alias, int option)
{
    auto it_tree = treeMap.find(treeName);
    if (it_tree == treeMap.end())
    {
        LOG_ERROR("AnalysisManager", "Cannot find the tree!");
        return false;
    }
    else
    {
        auto it = newBranchMap.find(alias);
        if (it == newBranchMap.end())
        {
            LOG_ERROR("AnalysisManager",
                      "No such variable in the new branch map : " << alias);
            return false;
        }
        double *ptr = newBranchData[alias];
        std::string name = newBranchMap[alias].realName;
        auto itold = branchMap.find(alias);
        if (option == TreeOpt::OM::kAppend)
        {
            if (itold == branchMap.end())
            {
                LOG_INFO("AnalysisManager", "New branch named "
                                                << name << " for " << treeName
                                                << " is appended.");
                it_tree->second->Branch(name.c_str(), ptr,
                                        (name + "/D").c_str());
                return true;
            }
            else
            {
                LOG_INFO("AnalysisManager",
                         "Cannot overload existing branch : " << alias);
                return false;
            }
        }
        else if (option == TreeOpt::OM::kRecreate)
        {
            LOG_INFO("AnalysisManager", "New branch named " << name << " for "
                                                            << treeName
                                                            << " is created.");
            it_tree->second->Branch(name.c_str(), ptr, (name + "/D").c_str());
            return true;
        }
        else
        {
            LOG_ERROR("AnalysisManager",
                      "Option should be kAppend or kRecreate!");
            return false;
        }
    }
}
void AnalysisManager::AddHist(const std::string &alias,
                              std::vector<double> binfo,
                              const std::string &prefix = "")
{
    std::string fullname = "hist_" + alias + "_" + prefix;
    bool chk = true;
    for (const auto &[alias, inmap] : histMap)
    {
        std::string existname = "hist_" + alias;
        for (const auto &[prefix, _] : inmap)
        {
            existname += "_" + prefix;
            if (existname == fullname)
            {
                chk = false;
            }
            if (!chk)
                break;
        }
        if (!chk)
            break;
    }
    if (chk)
    {
        LOG_INFO("AnalysisManager", "Histogram " << fullname << " is added");
        histData[alias][prefix] = static_cast<TH1 *>(
            new TH1D(fullname.c_str(), "", int(binfo[0]), binfo[1], binfo[2]));
        histData[alias][prefix]->SetDirectory(nullptr);
        histMap[alias][prefix] = binfo;
    }
    else
    {
        LOG_ERROR("AnalysisManager",
                  "The same histogram exists. name : " << fullname);
    }
}

void AnalysisManager::AddHist(const std::string &alias, TH1 *hist,
                              const std::string &prefix = "")
{
    std::string fullname = hist->GetName();
    bool chk = true;
    for (const auto &[alias, inmap] : histMap)
    {
        std::string existname = "hist_" + alias;
        for (const auto &[prefix, _] : inmap)
        {
            existname += "_" + prefix;
            if (existname == fullname)
            {
                chk = false;
            }
            if (!chk)
                break;
        }
        if (!chk)
            break;
    }
    if (chk)
    {
        LOG_INFO("AnalysisManager", "Histogram " << fullname << " is added");
        histData[alias][prefix] = hist;
        histData[alias][prefix]->SetDirectory(nullptr);
        double nbins = hist->GetNbinsX();
        double xmin = hist->GetBinLowEdge(1);
        double xmax = hist->GetBinLowEdge(hist->GetNbinsX()) +
                      hist->GetBinWidth(hist->GetNbinsX());
        histMap[alias][prefix] = {nbins, xmin, xmax};
    }
    else
    {
        LOG_ERROR("AnalysisManager",
                  "The same histogram exists. : " << fullname);
    }
}

void AnalysisManager::FillHists(double weight)
{
    for (const auto &[name, inmap] : histData)
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
    for (const auto &[_, inmap] : histData)
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

    for (const auto &[name, inmap] : histMap)
    {
        out << YAML::Key << name << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "expr" << YAML::Value << name;
        auto first_it = inmap.begin();
        out << YAML::Key << "bins" << YAML::Value << YAML::Flow
            << YAML::BeginSeq;
        for (auto b : first_it->second)
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
    if (i == 0)
        start_time = std::chrono::steady_clock::now();

    currentTree->GetEntry(i);

    if (i % 500 == 0 || i == GetEntries() - 1)
        UpdateProgress((double)(i + 1) / GetEntries());
}

Long64_t AnalysisManager::GetEntries() { return currentTree->GetEntries(); }

void AnalysisManager::SetRDFInput(const std::string &yamlPath)
{
    YAML::Node config = YAML::LoadFile(yamlPath);
    auto input = config["input"];
    if (input)
    {
        for (auto f : input["files"])
        {
            inputFiles.push_back(f.as<std::string>());
            LOG_INFO("AnalysisManager",
                     "File " << f.as<std::string>() << " has been loaded.");
        }
        inTreeName = input["tree"].as<std::string>();

        currentTree = new TChain(inTreeName.c_str());
        int count = 0;
        for (auto &file : inputFiles)
            count += currentTree->Add(file.c_str());

        if (count < 1)
        {
            LOG_ERROR("AnalysisManager", "This initializer is for the TTree!");
            return;
        }
        useRDF = true;
        rdf_raw = std::make_unique<ROOT::RDataFrame>(*currentTree);
        rdf_node = *rdf_raw;
        lm = std::make_unique<LambdaManager>();
    }
    else
    {
        LOG_ERROR("AnalysisManager", "Cannot find input!");
        return;
    }
}

void AnalysisManager::DefineRDFVar(const std::string &name,
                                   const std::string &expr)
{
    if (!useRDF)
        throw std::runtime_error("RDF not initialized");
    rdf_node = rdf_node->Define(name, expr);
    definedVars.insert(name);
}

void AnalysisManager::ApplyRDFFilter(const std::string &name)
{
    if (!useRDF)
        throw std::runtime_error("RDF not initialized");

    auto it = rawCutExpr.find(name);
    if (it != rawCutExpr.end())
    {
        if (it->second.find("--lambda:") == std::string::npos)
        {
            LOG_INFO("AnalysisManager",
                     "Cut " << name << " : " << it->second << " is applied.");
            rdf_node = rdf_node->Filter(it->second, name);
        }
    }
    else
        LOG_ERROR("AnalysisManager",
                  "Cannot find cut '" << name << "', filter is not applied.");
}

void AnalysisManager::ApplyRDFFilter(const std::string &name,
                                     const std::string &expr)
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
        rdf_node = rdf_node->Filter(expr, name);
        LOG_INFO("AnalysisManager",
                 "Direct filter is applied and registered. name : "
                     << name << " and expr : " << expr);
        rawCutExpr[name] = expr;
    }
}

void AnalysisManager::ApplyRDFFilterSelected(
    const std::vector<std::string> &names)
{
    for (const auto &n : names)
        ApplyRDFFilter(n);
}
void AnalysisManager::ApplyRDFFilterAll()
{
    for (const auto &[name, _] : rawCutExpr)
        ApplyRDFFilter(name);
}
void AnalysisManager::BookRDFHist1D(const std::string &alias,
                                    const std::string &prefix,
                                    std::vector<double> binfo)
{
    if (!rdf_node)
        throw std::runtime_error("RDF not initialized!");
    if (binfo.size() != 3)
    {
        LOG_ERROR("AnalysisManager",
                  "bin info should has form such as {100,0,1}");
        return;
    }
    std::string fullname = "hist_" + alias + "_" + prefix;
    auto model = ROOT::RDF::TH1DModel(fullname.c_str(), "", int(binfo[0]),
                                      binfo[1], binfo[2]);
    auto rptr = rdf_node->Histo1D(model, alias);
    histRDF[alias][prefix] = rptr;
}

void AnalysisManager::BookRDFHistsFromConfig(const std::string &yamlPath,
                                             const std::string &prefix)
{
    YAML::Node root = YAML::LoadFile(yamlPath);
    auto hists = root["histograms"];
    if (!hists)
        return;
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
            LOG_ERROR("AnalysisManager",
                      "Invalid bin format for histogram : " << alias);
        }
    }
}
void AnalysisManager::BookRDFHistsFromFile(const std::string &histfile)
{
    LoadHists(histfile);
    for (auto &[alias, inmap] : loadedHistMap)
    {
        for (auto &[prefix, binfo] : inmap)
        {
            BookRDFHist1D(alias, prefix, binfo);
        }
    }
}

void AnalysisManager::SnapshotRDF(const std::string &treeName,
                                  const std::string &fileName, int option)
{
    if (!useRDF)
        throw std::runtime_error("RDF not initialized");
    std::atomic<ULong64_t> counter = 0;
    auto callback = rdf_node->Count();

    callback.OnPartialResultSlot(500,
                                 [this, &counter](unsigned int j, ULong64_t &)
                                 {
                                     std::lock_guard<std::mutex> lock(prg_mtx);
                                     counter += 500;
                                     UpdateProgress(double(counter) /
                                                    GetEntries());
                                 });
    // ROOT::RDF::Experimental::AddProgressBar(*rdf_node);
    ROOT::RDF::RSnapshotOptions opts;
    opts.fLazy = true;
    std::vector<std::string> cols(definedVars.begin(), definedVars.end());
    if (option == TreeOpt::OM::kRecreate)
        rdf_node->Snapshot(treeName, fileName, cols, opts);
    else if (option == TreeOpt::OM::kAppend)
        rdf_node->Snapshot(treeName, fileName, rdf_node->GetColumnNames(),
                           opts);

    start_time = std::chrono::steady_clock::now();
    *callback;
    UpdateProgress(1.0);
}

void AnalysisManager::SaveHistsRDF(const std::string &outfile)
{
    if (!useRDF)
        throw std::runtime_error("RDF not initialized");

    std::atomic<ULong64_t> counter = 0;
    auto callback = rdf_node->Count();

    callback.OnPartialResultSlot(500,
                                 [this, &counter](unsigned int, ULong64_t &)
                                 {
                                     std::lock_guard<std::mutex> lock(prg_mtx);
                                     counter += 500;
                                     UpdateProgress(double(counter) /
                                                    GetEntries());
                                 });
    // ROOT::RDF::Experimental::AddProgressBar(*rdf_node);
    TFile *file = new TFile(outfile.c_str(), "recreate");
    file->cd();

    start_time = std::chrono::steady_clock::now();
    *callback;
    for (auto &[_, inmap] : histRDF)
    {
        for (auto &[_, hist] : inmap)
            hist->Write(hist->GetName(), TObject::kOverwrite);
    }
    file->Close();
    delete file;
    UpdateProgress(1.0);
    LOG_INFO("AnalysisManager", "RDF Histograms are saved in " << outfile);
}

LambdaManager *AnalysisManager::GetLambdaManager()
{
    if (!useRDF)
        throw std::runtime_error("RDF not initialized");
    return lm.get();
}

std::ofstream AnalysisManager::OpenResultFile(const std::string &filename,
                                              const std::string &mode) const
{
    std::ios_base::openmode open_mode = std::ios::out;
    TString tmode = mode;

    if (tmode.Contains("update", TString::kIgnoreCase))
    {
        LOG_INFO("AnalysisManager", filename << " is opened with update mode.");
        open_mode |= std::ios::app;
    }
    else
    {
        LOG_INFO("AnalysisManager", filename << " is opened.");
        open_mode |= std::ios::trunc;
    }

    std::ofstream fout(filename, open_mode);
    if (!fout)
        throw std::runtime_error("Failed to open result file : " + filename);
    return fout;
}

void AnalysisManager::UpdateProgress(double p)
{
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - start_time).count();
    double eta = (p > 0.0f && p < 1.0f) ? elapsed * (1.0f / p - 1.0f) : -1.0f;
    _progress = std::clamp(p, 0.0, 1.0);
    Logger::Get().PrintProgressBar("AnalysisManager", _progress, elapsed, eta);
}

std::vector<std::string> AnalysisManager::GetInputFiles() const
{
    return inputFiles;
}

std::map<std::string, std::string> AnalysisManager::GetCutExpressions() const
{
    return rawCutExpr;
}

void AnalysisManager::WriteMetaData(const std::string &filename,
                                    const std::string &hash,
                                    const std::string &basename,
                                    const std::string &paramjson)
{
    _hash = hash;
    _basename = basename;
    _paramjson = paramjson;
    _cuts = "";
    _endtime = Logger::Get().GetCurrentTime();
    for (const auto &[k, v] : rawCutExpr)
        _cuts += (k + ":" + v + ";");
    TFile fout(filename.c_str(), "UPDATE");
    TTree *tmeta = new TTree("metadata", "metadata");
    tmeta->Branch("hash", &_hash);
    tmeta->Branch("endtime", &_endtime);
    tmeta->Branch("module", &_basename);
    tmeta->Branch("paramjson", &_paramjson);
    tmeta->Branch("cuts", &_cuts);
    tmeta->Fill();
    tmeta->Write("metadata");
    fout.Close();
}
///////////////////////////////////////////////////////////////////////////////////////
void AnalysisManager::PrintCuts()
{
    LOG_INFO("AnalysisManager", "-------------REGISTERED CUTS----------------");
    for (const auto &[name, expr] : rawCutExpr)
        LOG_INFO("AnalysisManager", name << " : " << expr);
    LOG_INFO("AnalysisManager", "-------------REGISTERED CUTS----------------");
}

void AnalysisManager::PrintConfig()
{
    LOG_INFO("AnalysisManager",
             "-------------Configuration Summary------------");
    LOG_INFO("AnalysisManager", "FILES :");
    for (const auto &filename : inputFiles)
        LOG_INFO("AnalysisManager", "    " << filename);
    LOG_INFO("AnalysisManager", "    TREE : " << inTreeName);
    if (!useRDF)
    {
        LOG_INFO("AnalysisManager",
                 "        BRANCHES (ALIAS : BRANCH NAME / TYPE): ");
        for (const auto &[alias, binfo] : branchMap)
        {
            LOG_INFO("AnalysisManager", "               "
                                            << alias << " : " << binfo.realName
                                            << " / " << binfo.type);
        }
    }
    LOG_INFO("AnalysisManager",
             "-------------Configuration Summary-------------");
}

void AnalysisManager::PrintHists()
{
    LOG_INFO("AnalysisManager",
             "-------------Registered Histograms-------------");
    for (const auto &[alias, hists] : loadedHistMap)
    {
        for (const auto &[prefix, bins] : hists)
            LOG_INFO("AnalysisManager",
                     "loaded_hist_" << alias << "_" << prefix << " : " << "["
                                    << int(bins[0]) << "," << bins[1] << ","
                                    << bins[2] << "]");
    }

    for (const auto &[alias, hists] : histMap)
    {
        for (const auto &[prefix, bins] : hists)
            LOG_INFO("AnalysisManager", "hist_" << alias << "_" << prefix
                                                << " : " << "[" << int(bins[0])
                                                << "," << bins[1] << ","
                                                << bins[2] << "]");
    }
    if (useRDF)
    {
        for (const auto &[alias, hists] : histRDF)
            for (const auto &[prefix, _] : hists)
                LOG_INFO("AnalysisManager", "hist_" << alias << "_" << prefix
                                                    << " : RDF histograms");
    }
    LOG_INFO("AnalysisManager",
             "-------------Registered Histograms-------------");
}

AnalysisManager::AnalysisManager()
{
    void *handle = dlopen("libTreePlayer.so", RTLD_LAZY | RTLD_GLOBAL);
    if (!handle)
    {
        std::cerr << "[AnalysisManager] Failed to load libTreePlayer.so: "
                  << dlerror() << std::endl;
    }
    ROOT::EnableImplicitMT();
}

AnalysisManager::~AnalysisManager()
{
    for (auto &[_, tr] : treeMap)
        delete tr;
    treeMap.clear();
    for (auto &[alias, ptr] : branchData)
    {
        if (branchMap[alias].type == "Int_t")
            delete static_cast<int *>(ptr);
        else
            delete static_cast<double *>(ptr);
    }
    branchData.clear();
    branchMap.clear();
    newBranchMap.clear();
    for (auto &[_, ptr] : newBranchData)
        delete ptr;
    newBranchData.clear();
    histMap.clear();
    for (auto &[_, hists] : histData)
        for (auto &[_, h] : hists)
            delete h;
    histData.clear();
    loadedHistMap.clear();
    for (auto &[_, hists] : loadedHistData)
        for (auto &[_, h] : hists)
            delete h;
    loadedHistData.clear();
    histRDF.clear();
    rawCutExpr.clear();
    for (auto &[_, cut] : cutFormulas)
        delete cut;
    cutFormulas.clear();
    inputFiles.clear();
    delete currentTree;
    definedVars.clear();
}