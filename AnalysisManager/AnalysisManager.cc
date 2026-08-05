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
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <regex>
#include <sstream>
using namespace logger;

namespace
{
std::string EscapeRegex(const std::string &text)
{
    static const std::regex special(R"([-[\]{}()*+?.,\^$|#\s])");
    return std::regex_replace(text, special, R"(\$&)");
}

std::string SafeColumnName(std::string value)
{
    for (char &character : value)
        if (!std::isalnum(static_cast<unsigned char>(character)) && character != '_') character = '_';
    return value;
}

void DeleteBranchPointer(const std::string &type, void *pointer)
{
    if (type == "Double_t" || type == "double")
        delete static_cast<double *>(pointer);
    else if (type == "Float_t" || type == "float")
        delete static_cast<float *>(pointer);
    else if (type == "Int_t" || type == "int")
        delete static_cast<int *>(pointer);
    else if (type == "UInt_t" || type == "unsigned int")
        delete static_cast<unsigned int *>(pointer);
    else if (type == "Long64_t" || type == "long long")
        delete static_cast<Long64_t *>(pointer);
    else if (type == "ULong64_t" || type == "unsigned long long")
        delete static_cast<ULong64_t *>(pointer);
    else if (type == "Bool_t" || type == "bool")
        delete static_cast<bool *>(pointer);
}

void *AllocateBranchPointer(const std::string &type)
{
    if (type == "Double_t" || type == "double") return new double{};
    if (type == "Float_t" || type == "float") return new float{};
    if (type == "Int_t" || type == "int") return new int{};
    if (type == "UInt_t" || type == "unsigned int") return new unsigned int{};
    if (type == "Long64_t" || type == "long long") return new Long64_t{};
    if (type == "ULong64_t" || type == "unsigned long long") return new ULong64_t{};
    if (type == "Bool_t" || type == "bool") return new bool{};
    return nullptr;
}

double ReadBranchPointer(const std::string &type, const void *pointer)
{
    if (type == "Double_t" || type == "double") return *static_cast<const double *>(pointer);
    if (type == "Float_t" || type == "float") return *static_cast<const float *>(pointer);
    if (type == "Int_t" || type == "int") return *static_cast<const int *>(pointer);
    if (type == "UInt_t" || type == "unsigned int") return *static_cast<const unsigned int *>(pointer);
    if (type == "Long64_t" || type == "long long") return static_cast<double>(*static_cast<const Long64_t *>(pointer));
    if (type == "ULong64_t" || type == "unsigned long long") return static_cast<double>(*static_cast<const ULong64_t *>(pointer));
    if (type == "Bool_t" || type == "bool") return *static_cast<const bool *>(pointer) ? 1.0 : 0.0;
    throw std::runtime_error("AnalysisManager: unsupported branch type: " + type);
}

void ValidateHistogramBins(const std::vector<double> &bins, const std::string &name)
{
    if (bins.size() != 3 || !std::isfinite(bins[0]) || std::floor(bins[0]) != bins[0] || bins[0] <= 0 ||
        !std::isfinite(bins[1]) || !std::isfinite(bins[2]) || bins[2] <= bins[1])
        throw std::invalid_argument(
            "AnalysisManager: histogram '" + name + "' bins must be {integer_nbins, xmin, xmax} with a positive finite range.");
}

std::string CanonicalBranchType(const std::string &type)
{
    if (type == "Double_t" || type == "double") return "double";
    if (type == "Float_t" || type == "float") return "float";
    if (type == "Int_t" || type == "int") return "int";
    if (type == "UInt_t" || type == "unsigned int") return "unsigned int";
    if (type == "Long64_t" || type == "long long") return "long long";
    if (type == "ULong64_t" || type == "unsigned long long") return "unsigned long long";
    if (type == "Bool_t" || type == "bool") return "bool";
    return "";
}

bool IsSupportedBranchType(const std::string &type) { return !CanonicalBranchType(type).empty(); }

long long ProgressReportIntervalNs()
{
    static const long long interval = []()
    {
        const char *configured = std::getenv("CASCADE_PROGRESS_INTERVAL_MS");
        if (!configured || !*configured) return 200000000LL;
        char *end = nullptr;
        const long long milliseconds = std::strtoll(configured, &end, 10);
        if (end == configured || *end != '\0' || milliseconds < 0) return 200000000LL;
        return milliseconds * 1000000LL;
    }();
    return interval;
}

void ValidateSchemaVersion(const YAML::Node &root, ConfigValidationResult &result)
{
    if (!root || !root.IsMap())
    {
        result.Errors.push_back("document root must be a map");
        return;
    }
    const YAML::Node version = root["schema_version"];
    if (!version || !version.IsScalar())
    {
        result.Errors.push_back("schema_version is required");
        return;
    }
    try
    {
        const int value = version.as<int>();
        if (value != AnalysisManager::CONFIG_SCHEMA_VERSION)
            result.Errors.push_back("unsupported schema_version " + std::to_string(value) + "; expected " +
                                    std::to_string(AnalysisManager::CONFIG_SCHEMA_VERSION));
    }
    catch (const std::exception &)
    {
        result.Errors.push_back("schema_version must be an integer");
    }
}

YAML::Node LoadConfigForValidation(const std::string &path, ConfigValidationResult &result)
{
    try
    {
        return YAML::LoadFile(path);
    }
    catch (const std::exception &error)
    {
        result.Errors.push_back(std::string("cannot parse YAML: ") + error.what());
        return {};
    }
}
} // namespace

void ConfigValidationResult::ThrowIfInvalid(const std::string &configPath) const
{
    if (Valid()) return;
    std::ostringstream message;
    message << "AnalysisManager: config preflight failed for " << configPath;
    for (const auto &error : Errors)
        message << "\n - " << error;
    throw std::invalid_argument(message.str());
}

ConfigValidationResult AnalysisManager::PreflightInputConfig(const std::string &yamlPath) const
{
    ConfigValidationResult result;
    const YAML::Node config = LoadConfigForValidation(yamlPath, result);
    if (!config) return result;
    ValidateSchemaVersion(config, result);

    const YAML::Node input = config["input"];
    if (!input || !input.IsMap())
    {
        result.Errors.push_back("input must be a map");
        return result;
    }
    const YAML::Node files = input["files"];
    const YAML::Node treeNode = input["tree"];
    if (!files || !files.IsSequence() || files.size() == 0) result.Errors.push_back("input.files must be a non-empty sequence");
    if (!treeNode || !treeNode.IsScalar()) result.Errors.push_back("input.tree must be a non-empty string");

    const YAML::Node branches = config["branches"];
    if (!branches || !branches.IsMap()) result.Errors.push_back("branches must be a map");
    if (!result.Valid()) return result;

    std::string treeName;
    try
    {
        treeName = treeNode.as<std::string>();
        if (treeName.empty()) result.Errors.push_back("input.tree must be a non-empty string");
    }
    catch (const std::exception &)
    {
        result.Errors.push_back("input.tree must be a string");
    }

    struct RequestedBranch
    {
        std::string Alias;
        std::string Name;
        std::string Type;
    };
    std::vector<RequestedBranch> requested;
    for (const auto &entry : branches)
    {
        try
        {
            const std::string alias = entry.first.as<std::string>();
            const YAML::Node info = entry.second;
            if (alias.empty())
            {
                result.Errors.push_back("branch aliases cannot be empty");
                continue;
            }
            if (!info || !info.IsMap() || !info["name"] || !info["name"].IsScalar())
            {
                result.Errors.push_back("branches." + alias + ".name must be a non-empty string");
                continue;
            }
            RequestedBranch branch{alias, info["name"].as<std::string>(), ""};
            if (branch.Name.empty()) result.Errors.push_back("branches." + alias + ".name must be a non-empty string");
            if (info["type"])
            {
                if (!info["type"].IsScalar())
                    result.Errors.push_back("branches." + alias + ".type must be a string");
                else
                {
                    branch.Type = info["type"].as<std::string>();
                    if (!IsSupportedBranchType(branch.Type))
                        result.Errors.push_back("branches." + alias + ".type is unsupported: " + branch.Type);
                }
            }
            requested.push_back(std::move(branch));
        }
        catch (const std::exception &error)
        {
            result.Errors.push_back(std::string("invalid branch entry: ") + error.what());
        }
    }

    for (std::size_t index = 0; index < files.size(); ++index)
    {
        std::string filename;
        try
        {
            filename = files[index].as<std::string>();
        }
        catch (const std::exception &)
        {
            result.Errors.push_back("input.files[" + std::to_string(index) + "] must be a string");
            continue;
        }
        if (filename.empty())
        {
            result.Errors.push_back("input.files[" + std::to_string(index) + "] cannot be empty");
            continue;
        }
        std::unique_ptr<TFile> file(TFile::Open(filename.c_str(), "READ"));
        if (!file || file->IsZombie())
        {
            result.Errors.push_back("cannot open input file: " + filename);
            continue;
        }
        TTree *tree = nullptr;
        file->GetObject(treeName.c_str(), tree);
        if (!tree)
        {
            result.Errors.push_back("tree '" + treeName + "' is missing from " + filename);
            continue;
        }
        for (const auto &branch : requested)
        {
            TLeaf *leaf = tree->GetLeaf(branch.Name.c_str());
            if (!tree->GetBranch(branch.Name.c_str()) || !leaf)
            {
                result.Errors.push_back("branch '" + branch.Name + "' for alias '" + branch.Alias + "' is missing or non-scalar in " +
                                        filename);
                continue;
            }
            const std::string actualType = leaf->GetTypeName();
            if (!IsSupportedBranchType(actualType))
                result.Errors.push_back("branch '" + branch.Name + "' has unsupported type " + actualType + " in " + filename);
            if (!branch.Type.empty() && CanonicalBranchType(branch.Type) != CanonicalBranchType(actualType))
                result.Errors.push_back("branch '" + branch.Name + "' type mismatch in " + filename + ": configured " + branch.Type +
                                        ", actual " + actualType);
        }
    }
    return result;
}

ConfigValidationResult AnalysisManager::PreflightCutConfig(const std::string &yamlPath) const
{
    ConfigValidationResult result;
    const YAML::Node config = LoadConfigForValidation(yamlPath, result);
    if (!config) return result;
    ValidateSchemaVersion(config, result);
    const YAML::Node cuts = config["cuts"];
    if (!cuts || !cuts.IsMap())
    {
        result.Errors.push_back("cuts must be a map");
        return result;
    }
    for (const auto &entry : cuts)
    {
        try
        {
            const std::string name = entry.first.as<std::string>();
            if (name.empty() || !entry.second.IsScalar())
            {
                result.Errors.push_back("cut names must be non-empty and expressions must be strings");
                continue;
            }
            const std::string expression = entry.second.as<std::string>();
            if (expression.empty())
            {
                result.Errors.push_back("cuts." + name + " cannot be empty");
                continue;
            }
            if (expression.rfind("--lambda:", 0) == 0)
            {
                result.Errors.push_back("cuts." + name + " cannot deserialize a lambda filter");
                continue;
            }
            if (m_CurrentTree)
            {
                TTreeFormula formula(("cascade_preflight_cut_" + SafeColumnName(name)).c_str(), ExpandAliases_(expression).c_str(),
                                     m_CurrentTree);
                if (formula.GetNdim() <= 0) result.Errors.push_back("cuts." + name + " is not a valid tree expression");
            }
        }
        catch (const std::exception &error)
        {
            result.Errors.push_back(std::string("invalid cut entry: ") + error.what());
        }
    }
    return result;
}

ConfigValidationResult AnalysisManager::PreflightHistogramConfig(const std::string &yamlPath) const
{
    ConfigValidationResult result;
    const YAML::Node config = LoadConfigForValidation(yamlPath, result);
    if (!config) return result;
    ValidateSchemaVersion(config, result);
    const YAML::Node histograms = config["histograms"];
    if (!histograms || !histograms.IsMap())
    {
        result.Errors.push_back("histograms must be a map");
        return result;
    }
    for (const auto &entry : histograms)
    {
        try
        {
            const std::string name = entry.first.as<std::string>();
            const YAML::Node info = entry.second;
            if (name.empty() || !info || !info.IsMap())
            {
                result.Errors.push_back("histogram entries must be named maps");
                continue;
            }
            if (!info["expr"] || !info["expr"].IsScalar())
            {
                result.Errors.push_back("histograms." + name + ".expr must be a non-empty string");
                continue;
            }
            const std::string expression = info["expr"].as<std::string>();
            if (expression.empty()) result.Errors.push_back("histograms." + name + ".expr cannot be empty");
            const YAML::Node bins = info["bins"];
            if (!bins || !bins.IsSequence() || bins.size() != 3)
                result.Errors.push_back("histograms." + name + ".bins must contain [nbins, xmin, xmax]");
            else
            {
                try
                {
                    ValidateHistogramBins({bins[0].as<double>(), bins[1].as<double>(), bins[2].as<double>()}, name);
                }
                catch (const std::exception &error)
                {
                    result.Errors.push_back(error.what());
                }
            }
            if (m_CurrentTree && !expression.empty())
            {
                TTreeFormula formula(("cascade_preflight_hist_" + SafeColumnName(name)).c_str(), ExpandAliases_(expression).c_str(),
                                     m_CurrentTree);
                if (formula.GetNdim() <= 0) result.Errors.push_back("histograms." + name + ".expr is not a valid tree expression");
            }
        }
        catch (const std::exception &error)
        {
            result.Errors.push_back(std::string("invalid histogram entry: ") + error.what());
        }
    }
    return result;
}

void AnalysisManager::LoadInputConfig(const std::string &yamlPath)
{
    PreflightInputConfig(yamlPath).ThrowIfInvalid(yamlPath);
    ReleaseCurrentTree_();
    m_InputFiles.clear();
    m_BranchMap.clear();
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

void AnalysisManager::LoadCutConfig(const std::string &yamlPath)
{
    if (m_UseRdf && !m_AppliedRdfCuts.empty())
        throw std::runtime_error("AnalysisManager: cannot replace cut configuration after RDF filters were applied.");
    PreflightCutConfig(yamlPath).ThrowIfInvalid(yamlPath);
    for (auto &[_, formula] : m_CutFormulas)
        delete formula;
    m_CutFormulas.clear();
    m_RawCutExpr.clear();
    m_AppliedRdfCuts.clear();
    YAML::Node cutsNode = YAML::LoadFile(yamlPath)["cuts"];
    for (auto it : cutsNode)
    {
        std::string name = it.first.as<std::string>();
        std::string rawExpr = it.second.as<std::string>();
        m_RawCutExpr[name] = rawExpr;
        LOG_INFO("AnalysisManager", "Registered cut '" << name << "' from " << yamlPath);
    }
    LOG_INFO("AnalysisManager", "Cut named " << yamlPath << " has been loaded.");
}

TChain *AnalysisManager::BuildChain()
{
    ReleaseCurrentTree_();
    m_CurrentTreeOwner = std::make_shared<TChain>(m_InTreeName.c_str());
    m_CurrentTree = m_CurrentTreeOwner.get();
    int count = 0;
    for (auto &file : m_InputFiles)
        count += m_CurrentTree->Add(file.c_str());

    if (count < 1)
    {
        LOG_ERROR("AnalysisManager", "This initializer is for the TTree!");
        ReleaseCurrentTree_();
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
                throw std::runtime_error("AnalysisManager: branch has no scalar leaf: " + realName);
            }
            type = leaf->GetTypeName();
            info.Type = type;
        }

        void *pointer = AllocateBranchPointer(type);
        if (!pointer)
        {
            throw std::runtime_error("AnalysisManager: unsupported branch type for '" + alias + "': " + type);
        }
        if (m_CurrentTree->SetBranchAddress(realName.c_str(), pointer) < 0)
        {
            DeleteBranchPointer(type, pointer);
            throw std::runtime_error("AnalysisManager: failed to attach branch '" + realName + "' for alias '" + alias + "'.");
        }
        m_BranchData[alias] = pointer;
    }
    LOG_INFO("AnalysisManager", "Tree " << m_CurrentTree->GetName() << " is initialized.");

    return m_CurrentTree;
}

void AnalysisManager::RegisterTree(const std::string &name)
{
    auto it = m_TreeMap.find(name);
    if (it == m_TreeMap.end())
    {
        TTree *tree = new TTree(name.c_str(), name.c_str());
        tree->SetDirectory(nullptr);
        m_TreeMap[name] = tree;
        m_TreeOwnership[name] = ResourceOwnership::Owned;
        LOG_INFO("AnalysisManager", "Following tree is added to the lists : " << name);
    }
    else
    {
        throw std::runtime_error("AnalysisManager: tree already exists: " + name);
    }
}

void AnalysisManager::RegisterTree(TTree *tree, ResourceOwnership ownership)
{
    if (!tree) throw std::invalid_argument("AnalysisManager: cannot register a null tree.");
    std::string name = tree->GetName();
    auto it = m_TreeMap.find(name);
    if (it == m_TreeMap.end())
    {
        if (ownership == ResourceOwnership::Owned) tree->SetDirectory(nullptr);
        m_TreeMap[name] = tree;
        m_TreeOwnership[name] = ownership;
        LOG_INFO("AnalysisManager", "Following tree is added to the lists : " << name);
    }
    else
    {
        throw std::runtime_error("AnalysisManager: tree already exists: " + name);
    }
}
void AnalysisManager::WriteTrees(const std::string &outfile)
{
    TFile file(outfile.c_str(), "recreate");
    if (file.IsZombie()) throw std::runtime_error("AnalysisManager: cannot create tree output file: " + outfile);
    file.cd();
    for (const auto &[_, tree] : m_TreeMap)
    {
        if (tree->Write(tree->GetName(), TObject::kOverwrite) < 0)
            throw std::runtime_error("AnalysisManager: failed to write tree: " + std::string(tree->GetName()));
    }
    file.Close();
    LOG_INFO("AnalysisManager", "Trees are saved in " << outfile);
}
void AnalysisManager::LoadHistogramConfig(const std::string &yamlPath, const std::string &prefix)
{
    PreflightHistogramConfig(yamlPath).ThrowIfInvalid(yamlPath);
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
            const std::vector<double> binInfo{double(nbins), xmin, xmax};
            ValidateHistogramBins(binInfo, alias);
            auto formulaAlias = m_HistFormulas.find(alias);
            if (formulaAlias != m_HistFormulas.end())
            {
                auto formula = formulaAlias->second.find(prefix);
                if (formula != formulaAlias->second.end())
                {
                    delete formula->second;
                    formulaAlias->second.erase(formula);
                }
            }
            if (m_HistData.count(alias) && m_HistData.at(alias).count(prefix) &&
                m_HistOwnership[alias][prefix] == ResourceOwnership::Owned)
                delete m_HistData[alias][prefix];
            m_HistMap[alias][prefix] = binInfo;
            m_HistData[alias][prefix] = static_cast<TH1 *>(new TH1D(("hist_" + alias + "_" + prefix).c_str(), expr.c_str(), nbins, xmin, xmax));
            m_HistData[alias][prefix]->SetDirectory(nullptr);
            m_HistOwnership[alias][prefix] = ResourceOwnership::Owned;
            m_HistExpressions[alias][prefix] = expr;
        }
        else
        {
            throw std::invalid_argument("AnalysisManager: invalid bin format for histogram '" + alias + "'.");
        }
    }
}

void AnalysisManager::LoadHistogramTemplateFile(const std::string &histfile)
{
    LoadHists_(histfile);
    for (auto &[name, inmap] : m_LoadedHistMap)
        for (auto &[prefix, binfo] : inmap)
            m_HistMap[name][prefix] = binfo;
    for (auto &[name, inmap] : m_LoadedHistData)
    {
        for (auto &[prefix, hist] : inmap)
        {
            if (m_HistData.count(name) && m_HistData.at(name).count(prefix) &&
                m_HistOwnership[name][prefix] == ResourceOwnership::Owned)
                delete m_HistData[name][prefix];
            if (m_HistFormulas.count(name) && m_HistFormulas.at(name).count(prefix))
            {
                delete m_HistFormulas[name][prefix];
                m_HistFormulas[name].erase(prefix);
            }
            TString chName = hist->GetName();
            chName = chName.Remove(0, 7);
            m_HistData[name][prefix] = static_cast<TH1 *>(hist->Clone(chName));
            m_HistData[name][prefix]->Reset();
            m_HistData[name][prefix]->SetDirectory(nullptr);
            m_HistOwnership[name][prefix] = ResourceOwnership::Owned;
            m_HistExpressions[name][prefix] = name;
        }
    }
}
void AnalysisManager::LoadHists_(const std::string &histfile)
{
    LOG_INFO("AnalysisManager", "Loading histograms from " << histfile);
    for (auto &[_, histograms] : m_LoadedHistData)
        for (auto &[__, histogram] : histograms)
            delete histogram;
    m_LoadedHistData.clear();
    m_LoadedHistMap.clear();

    TFile *file = new TFile(histfile.c_str());
    if (!file || file->IsZombie())
    {
        delete file;
        throw std::runtime_error("AnalysisManager: failed to open histogram file: " + histfile);
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

void AnalysisManager::EnableCuts(const std::vector<std::string> &selected)
{
    if (!m_CurrentTree || m_UseRdf)
        throw std::runtime_error("AnalysisManager: classic cuts require an initialized non-RDF tree.");
    for (const auto &name : selected)
        if (!m_RawCutExpr.count(name)) throw std::runtime_error("AnalysisManager: cut is not registered: " + name);
    LOG_INFO("AnalysisManager", "Activating selected cuts" << (selected.empty() ? std::string(" (all available)") : std::string("")));
    for (const auto &[name, expr] : m_RawCutExpr)
    {
        if (!selected.empty() && std::find(selected.begin(), selected.end(), name) == selected.end()) continue;
        if (m_CutFormulas.count(name)) delete m_CutFormulas[name];
        m_CutFormulas[name] = new TTreeFormula(name.c_str(), ExpandAliases_(expr).c_str(), m_CurrentTree);
        LOG_INFO("AnalysisManager", "Cut activated: " << name << " => " << expr);
    }
}

void AnalysisManager::EnableAllCuts()
{
    if (!m_CurrentTree || m_UseRdf)
        throw std::runtime_error("AnalysisManager: classic cuts require an initialized non-RDF tree.");
    for (const auto &[name, expr] : m_RawCutExpr)
    {
        if (m_CutFormulas.count(name)) delete m_CutFormulas[name];
        m_CutFormulas[name] = new TTreeFormula(name.c_str(), ExpandAliases_(expr).c_str(), m_CurrentTree);
    }
    LOG_INFO("AnalysisManager", "All Cuts are activated!");
}

std::string AnalysisManager::ExpandAliases_(const std::string &expr) const
{
    std::string result = expr;
    for (const auto &[alias, binfo] : m_BranchMap)
    {
        std::regex pattern("\\b" + EscapeRegex(alias) + "\\b");
        result = std::regex_replace(result, pattern, binfo.RealName);
    }
    return result;
}

void AnalysisManager::RegisterCut(const std::string &name, const std::string &expr)
{
    if (m_UseRdf && m_AppliedRdfCuts.count(name))
        throw std::runtime_error("AnalysisManager: cannot replace an RDF cut after it was applied: " + name);
    auto formula = m_CutFormulas.find(name);
    if (formula != m_CutFormulas.end())
    {
        delete formula->second;
        m_CutFormulas.erase(formula);
    }
    m_RawCutExpr[name] = expr;
    LOG_INFO("AnalysisManager", "Cut added: " << name << " -> " << expr);
}

bool AnalysisManager::PassesCut(const std::string &name) const
{
    auto it = m_CutFormulas.find(name);
    if (it == m_CutFormulas.end()) throw std::runtime_error("AnalysisManager: cut is not enabled: " + name);
    return it->second->EvalInstance();
}

bool AnalysisManager::PassesCuts(const std::vector<std::string> &names)
{
    for (const auto &name : names)
        if (!PassesCut(name)) return false;
    return true;
}

bool AnalysisManager::PassesCuts(std::initializer_list<std::string> names)
{
    for (const auto &name : names)
        if (!PassesCut(name)) return false;
    return true;
}

bool AnalysisManager::PassesAllCuts()
{
    for (const auto &[name, _] : m_CutFormulas)
        if (!PassesCut(name)) return false;
    return true;
}

double AnalysisManager::GetValue(const std::string &alias) const
{
    auto it = m_BranchData.find(alias);
    if (it == m_BranchData.end())
    {
        return GetNewVar_(alias);
    }
    else
    {
        return ReadBranchPointer(m_BranchMap.at(alias).Type, m_BranchData.at(alias));
    }
}

double AnalysisManager::GetNewVar_(const std::string &alias) const
{
    auto it = m_NewBranchData.find(alias);
    if (it == m_NewBranchData.end())
        throw std::runtime_error("AnalysisManager: variable is not registered: " + alias);
    return *m_NewBranchData.at(alias);
}

std::string AnalysisManager::GetCutExpression(const std::string &name) const
{
    auto it = m_RawCutExpr.find(name);
    if (it != m_RawCutExpr.end())
        return it->second;
    throw std::runtime_error("AnalysisManager: cut is not registered: " + name);
}

void AnalysisManager::WriteCutConfig(const std::string &yamlPath) const
{
    YAML::Emitter out;
    out << YAML::BeginMap;
    out << YAML::Key << "schema_version" << YAML::Value << CONFIG_SCHEMA_VERSION;
    out << YAML::Key << "cuts" << YAML::Value << YAML::BeginMap;
    for (const auto &[name, expr] : m_RawCutExpr)
    {
        if (expr.rfind("--lambda:", 0) == 0)
            throw std::runtime_error("AnalysisManager: lambda cut cannot be serialized: " + name);
        out << YAML::Key << name << YAML::Value << expr;
    }
    out << YAML::EndMap << YAML::EndMap;
    std::ofstream fout(yamlPath);
    if (!fout) throw std::runtime_error("AnalysisManager: unable to open cut output file: " + yamlPath);

    fout << out.c_str();
    if (!fout) throw std::runtime_error("AnalysisManager: unable to write cut output file: " + yamlPath);
    LOG_INFO("AnalysisManager", "Cuts are saved to " << yamlPath);
}

void AnalysisManager::WriteInputConfig(TTree *tree, const std::string &yamlOut, const std::vector<std::string> &filenames)
{
    if (!tree) throw std::invalid_argument("AnalysisManager: cannot generate input config from a null tree.");
    LOG_INFO("AnalysisManager", "Generating configuration for tree " << tree->GetName() << " to " << yamlOut);
    YAML::Emitter out;
    out << YAML::BeginMap;
    out << YAML::Key << "schema_version" << YAML::Value << CONFIG_SCHEMA_VERSION;

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
        TLeaf *leaf = b->GetLeaf(name.c_str());
        if (!leaf && b->GetListOfLeaves() && b->GetListOfLeaves()->GetEntries() > 0)
            leaf = static_cast<TLeaf *>(b->GetListOfLeaves()->At(0));

        out << YAML::Key << name << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "name" << YAML::Value << name;
        if (leaf) out << YAML::Key << "type" << YAML::Value << leaf->GetTypeName();
        out << YAML::EndMap;
    }
    out << YAML::EndMap;

    out << YAML::EndMap;

    std::ofstream fout(yamlOut);
    if (!fout) throw std::runtime_error("AnalysisManager: unable to open generated config: " + yamlOut);

    fout << out.c_str();
    if (!fout) throw std::runtime_error("AnalysisManager: unable to write generated config: " + yamlOut);
    fout.close();
    LOG_INFO("AnalysisManager", "Configuration is generated at " << yamlOut);
}

double *AnalysisManager::RegisterVariable(const std::string &name, std::string alias)
{
    if (alias.length() == 0) alias = name;

    auto it = m_NewBranchMap.find(alias);
    if (it != m_NewBranchMap.end())
        throw std::runtime_error("AnalysisManager: variable already exists: " + alias);

    double *ptr = new double;
    m_NewBranchMap[alias] = {name, "Double_t"};
    m_NewBranchData[alias] = ptr;
    LOG_INFO("AnalysisManager", "New variable added: " << alias << " mapped to " << name);
    return ptr;
}

bool AnalysisManager::AttachBranch(TTree *tree, const std::string &alias, TreeOpt::Om option)
{
    if (!tree)
    {
        LOG_ERROR("AnalysisManager", "Cannot attach a branch to a null tree.");
        return false;
    }
    if (tree == m_CurrentTree)
    {
        LOG_ERROR("AnalysisManager", "Cannot attach a branch to the current input tree.");
        return false;
    }

    const auto variable = m_NewBranchMap.find(alias);
    if (variable == m_NewBranchMap.end())
    {
        LOG_ERROR("AnalysisManager", "Variable is not registered: " << alias);
        return false;
    }
    if (option != TreeOpt::Om::Append && option != TreeOpt::Om::Recreate)
    {
        LOG_ERROR("AnalysisManager", "Branch option must be Append or Recreate.");
        return false;
    }

    const std::string &name = variable->second.RealName;
    if (tree->GetBranch(name.c_str()))
    {
        LOG_ERROR("AnalysisManager", "Replacing an existing TTree branch is not supported: " << name);
        return false;
    }

    double *value = m_NewBranchData.at(alias);
    TBranch *branch = tree->Branch(name.c_str(), value, (name + "/D").c_str());
    if (!branch)
    {
        LOG_ERROR("AnalysisManager", "Failed to attach branch '" << name << "' to tree '" << tree->GetName() << "'.");
        return false;
    }
    LOG_INFO("AnalysisManager", "Branch '" << name << "' was attached to tree '" << tree->GetName() << "'.");
    return true;
}

bool AnalysisManager::AttachBranch(const std::string &treeName, const std::string &alias, TreeOpt::Om option)
{
    const auto tree = m_TreeMap.find(treeName);
    if (tree == m_TreeMap.end())
    {
        LOG_ERROR("AnalysisManager", "Tree is not registered: " << treeName);
        return false;
    }
    return AttachBranch(tree->second, alias, option);
}
void AnalysisManager::BookHistogram(const std::string &alias, std::vector<double> binfo, const std::string &prefix)
{
    ValidateHistogramBins(binfo, alias);
    std::string fullname = "hist_" + alias + "_" + prefix;
    if (m_HistData.count(alias) && m_HistData.at(alias).count(prefix))
        throw std::runtime_error("AnalysisManager: histogram already exists: " + fullname);

    LOG_INFO("AnalysisManager", "Histogram " << fullname << " is added");
    m_HistData[alias][prefix] = static_cast<TH1 *>(new TH1D(fullname.c_str(), "", int(binfo[0]), binfo[1], binfo[2]));
    m_HistData[alias][prefix]->SetDirectory(nullptr);
    m_HistMap[alias][prefix] = std::move(binfo);
    m_HistOwnership[alias][prefix] = ResourceOwnership::Owned;
    m_HistExpressions[alias][prefix] = alias;
}

void AnalysisManager::RegisterHistogram(const std::string &alias, TH1 *hist, const std::string &prefix, ResourceOwnership ownership)
{
    if (!hist) throw std::invalid_argument("AnalysisManager: cannot register a null histogram.");
    std::string fullname = hist->GetName();
    if (m_HistData.count(alias) && m_HistData.at(alias).count(prefix))
        throw std::runtime_error("AnalysisManager: histogram already registered for alias/prefix: " + alias + "/" + prefix);

    LOG_INFO("AnalysisManager", "Histogram " << fullname << " is added");
    m_HistData[alias][prefix] = hist;
    if (ownership == ResourceOwnership::Owned) hist->SetDirectory(nullptr);
    double nbins = hist->GetNbinsX();
    double xmin = hist->GetBinLowEdge(1);
    double xmax = hist->GetBinLowEdge(hist->GetNbinsX()) + hist->GetBinWidth(hist->GetNbinsX());
    m_HistMap[alias][prefix] = {nbins, xmin, xmax};
    m_HistOwnership[alias][prefix] = ownership;
    m_HistExpressions[alias][prefix] = alias;
}

void AnalysisManager::FillHistograms(double weight)
{
    if (!m_CurrentTree) throw std::runtime_error("AnalysisManager: cannot fill histograms before a tree is initialized.");
    for (const auto &[alias, inmap] : m_HistData)
    {
        for (const auto &[prefix, hist] : inmap)
        {
            const std::string expression =
                m_HistExpressions.count(alias) && m_HistExpressions.at(alias).count(prefix) ? m_HistExpressions.at(alias).at(prefix) : alias;
            double value = 0.0;
            if (m_BranchData.count(expression) || m_NewBranchData.count(expression))
            {
                value = GetValue(expression);
            }
            else
            {
                auto &formula = m_HistFormulas[alias][prefix];
                if (!formula)
                {
                    const std::string formulaName = "cascade_hist_formula_" + SafeColumnName(alias + "_" + prefix);
                    formula = new TTreeFormula(formulaName.c_str(), ExpandAliases_(expression).c_str(), m_CurrentTree);
                    if (formula->GetNdim() <= 0)
                    {
                        delete formula;
                        formula = nullptr;
                        throw std::runtime_error("AnalysisManager: invalid histogram expression for '" + alias + "': " + expression);
                    }
                }
                value = formula->EvalInstance();
            }
            hist->Fill(value, weight);
        }
    }
}

void AnalysisManager::WriteHistograms(const std::string &outfile)
{
    TFile file(outfile.c_str(), "recreate");
    if (file.IsZombie()) throw std::runtime_error("AnalysisManager: cannot create histogram output file: " + outfile);
    file.cd();
    for (const auto &[_, inmap] : m_HistData)
    {
        for (const auto &[_, hist] : inmap)
            if (hist->Write(hist->GetName(), TObject::kOverwrite) < 0)
                throw std::runtime_error("AnalysisManager: failed to write histogram: " + std::string(hist->GetName()));
    }
    file.Close();
    LOG_INFO("AnalysisManager", "Histograms are saved in " << outfile);
}

void AnalysisManager::WriteHistogramConfig(const std::string &yamlOut)
{
    YAML::Emitter out;
    out << YAML::BeginMap;
    out << YAML::Key << "schema_version" << YAML::Value << CONFIG_SCHEMA_VERSION;

    out << YAML::Key << "histograms" << YAML::Value << YAML::BeginMap;

    for (const auto &[name, inmap] : m_HistMap)
    {
        out << YAML::Key << name << YAML::Value << YAML::BeginMap;
        const std::string &prefix = inmap.begin()->first;
        const std::string expression =
            m_HistExpressions.count(name) && m_HistExpressions.at(name).count(prefix) ? m_HistExpressions.at(name).at(prefix) : name;
        out << YAML::Key << "expr" << YAML::Value << expression;
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
    if (!fout) throw std::runtime_error("AnalysisManager: unable to open histogram config output: " + yamlOut);
    fout << out.c_str();
    if (!fout) throw std::runtime_error("AnalysisManager: unable to write histogram config output: " + yamlOut);
    fout.close();
}

void AnalysisManager::LoadEvent(Long64_t i)
{
    if (!m_CurrentTree) throw std::runtime_error("AnalysisManager: no input tree is initialized.");
    const Long64_t entries = GetEntryCount();
    if (i < 0 || i >= entries) throw std::out_of_range("AnalysisManager: event index is outside the input range.");
    if (i == 0) m_StartTime = std::chrono::steady_clock::now();

    m_CurrentTree->GetEntry(i);

    if (i % 500 == 0 || i == entries - 1) UpdateProgress_((double)(i + 1) / entries);
}

Long64_t AnalysisManager::GetEntryCount()
{
    if (!m_CurrentTree) throw std::runtime_error("AnalysisManager: no input tree is initialized.");
    return m_CurrentTree->GetEntries();
}

void AnalysisManager::InitRdfFromConfig(const std::string &yamlPath)
{
    LoadInputConfig(yamlPath);
    if (m_InputFiles.empty() || m_InTreeName.empty()) throw std::runtime_error("AnalysisManager: RDF input config is incomplete.");
    if (!BuildChain()) throw std::runtime_error("AnalysisManager: RDF input files did not provide the requested tree.");

    m_UseRdf = true;
    m_RdfRaw = std::make_unique<ROOT::RDataFrame>(*m_CurrentTree);
    m_RdfNode = *m_RdfRaw;
    for (const auto &[alias, info] : m_BranchMap)
    {
        if (alias != info.RealName) m_RdfNode = m_RdfNode->Define(alias, info.RealName);
    }
    m_LambdaManager = std::make_unique<LambdaManager>();
    LOG_INFO("AnalysisManager", "RDF input initialized from config " << yamlPath << " with " << m_InputFiles.size()
                                  << " files for tree " << m_InTreeName);
}

void AnalysisManager::InitRdfFromFile(const std::string &treename, const std::string &filename)
{
    ReleaseCurrentTree_();
    m_BranchMap.clear();
    m_InputFiles = {filename};
    m_InTreeName = treename;
    m_CurrentTreeOwner = std::make_shared<TChain>(treename.c_str());
    m_CurrentTree = m_CurrentTreeOwner.get();
    if (m_CurrentTree->Add(filename.c_str()) < 1)
    {
        ReleaseCurrentTree_();
        throw std::runtime_error("AnalysisManager: failed to add RDF input file: " + filename);
    }
    m_UseRdf = true;
    m_RdfRaw = std::make_unique<ROOT::RDataFrame>(*m_CurrentTree);
    m_RdfNode = *m_RdfRaw;
    m_LambdaManager = std::make_unique<LambdaManager>();
    LOG_INFO("AnalysisManager", "RDF input initialized from file " << filename << " for tree " << treename);
}

void AnalysisManager::DefineRdfVariable(const std::string &name, const std::string &expr)
{
    if (!m_UseRdf) throw std::runtime_error("RDF not initialized");
    m_RdfNode = m_RdfNode->Define(name, expr);
    LOG_INFO("AnalysisManager", "Defined RDF variable '" << name << "' with expression '" << expr << "'");
}

void AnalysisManager::ApplyRdfFilter(const std::string &name)
{
    if (!m_UseRdf) throw std::runtime_error("RDF not initialized");
    if (m_AppliedRdfCuts.count(name)) return;

    auto it = m_RawCutExpr.find(name);
    if (it != m_RawCutExpr.end())
    {
        if (it->second.find("--lambda:") == std::string::npos)
        {
            LOG_INFO("AnalysisManager", "Cut " << name << " : " << it->second << " is applied.");
            m_RdfNode = m_RdfNode->Filter(ExpandAliases_(it->second), name);
            m_AppliedRdfCuts.insert(name);
        }
        else
            throw std::runtime_error("AnalysisManager: lambda cut has no callable implementation: " + name);
    }
    else
        throw std::runtime_error("AnalysisManager: RDF cut is not registered: " + name);
}

void AnalysisManager::ApplyRdfFilter(const std::string &name, const std::string &expr)
{
    if (!m_UseRdf) throw std::runtime_error("RDF not initialized");
    auto it = m_RawCutExpr.find(name);
    if (it != m_RawCutExpr.end())
        throw std::runtime_error("AnalysisManager: RDF cut already exists: " + name);
    else
    {
        m_RdfNode = m_RdfNode->Filter(expr, name);
        LOG_INFO("AnalysisManager", "Direct filter is applied and registered. name : " << name << " and expr : " << expr);
        m_RawCutExpr[name] = expr;
        m_AppliedRdfCuts.insert(name);
    }
}

void AnalysisManager::ApplyRdfFilters(const std::vector<std::string> &names)
{
    LOG_INFO("AnalysisManager", "Applying RDF filters for selected cuts (" << names.size() << " entries)");
    for (const auto &n : names)
        ApplyRdfFilter(n);
}
void AnalysisManager::ApplyAllRdfFilters()
{
    LOG_INFO("AnalysisManager", "Applying RDF filters for all registered cuts");
    for (const auto &[name, _] : m_RawCutExpr)
        ApplyRdfFilter(name);
}
void AnalysisManager::BookRdfHistogram1D(const std::string &alias, const std::string &prefix, std::vector<double> binfo,
                                         const std::string &expression)
{
    if (!m_RdfNode) throw std::runtime_error("RDF not initialized!");
    ValidateHistogramBins(binfo, alias);
    std::string fullname = "hist_" + alias + "_" + prefix;
    if (m_HistRdf.count(alias) && m_HistRdf.at(alias).count(prefix))
        throw std::runtime_error("AnalysisManager: RDF histogram already exists: " + fullname);
    std::string column = expression.empty() ? alias : expression;
    const auto columns = m_RdfNode->GetColumnNames();
    if (std::find(columns.begin(), columns.end(), column) == columns.end())
    {
        const std::string internalColumn = "__cascade_hist_" + SafeColumnName(alias + "_" + prefix);
        m_RdfNode = m_RdfNode->Define(internalColumn, ExpandAliases_(column));
        column = internalColumn;
    }
    auto model = ROOT::RDF::TH1DModel(fullname.c_str(), "", int(binfo[0]), binfo[1], binfo[2]);
    auto rptr = m_RdfNode->Histo1D(model, column);
    m_HistRdf[alias][prefix] = rptr;
    m_HistMap[alias][prefix] = binfo;
    m_HistExpressions[alias][prefix] = expression.empty() ? alias : expression;
    LOG_INFO("AnalysisManager", "Booked RDF histogram '" << fullname << "' with bins {" << binfo[0] << ", " << binfo[1] << ", "
                                           << binfo[2] << "}");
}

void AnalysisManager::BookRdfHistogramsFromConfig(const std::string &yamlPath, const std::string &prefix)
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
            BookRdfHistogram1D(alias, prefix, {double(nbins), xmin, xmax}, expr);
        }
        else
        {
            throw std::invalid_argument("AnalysisManager: invalid bin format for RDF histogram '" + alias + "'.");
        }
    }
    LOG_INFO("AnalysisManager", "Booked RDF histograms from config " << yamlPath << " with prefix '" << prefix << "'");
}
void AnalysisManager::BookRdfHistogramsFromFile(const std::string &histfile)
{
    LoadHists_(histfile);
    for (auto &[alias, inmap] : m_LoadedHistMap)
    {
        for (auto &[prefix, binfo] : inmap)
        {
            BookRdfHistogram1D(alias, prefix, binfo, alias);
        }
    }
    LOG_INFO("AnalysisManager", "Booked RDF histograms based on file " << histfile);
}

void AnalysisManager::WriteRdfSnapshot(const std::string &treeName, const std::string &fileName, TreeOpt::Om option)
{
    if (!m_UseRdf) throw std::runtime_error("RDF not initialized");
    std::atomic<ULong64_t> counter = 0;
    const double entryCount = static_cast<double>(GetEntryCount());
    auto callback = m_RdfNode->Count();

    callback.OnPartialResultSlot(500,
                                 [this, &counter, entryCount](unsigned int, ULong64_t &)
                                 {
                                     const auto processed = counter.fetch_add(500) + 500;
                                     UpdateProgress_(double(processed) / entryCount);
                                 });
    // ROOT::RDF::Experimental::AddProgressBar(*m_RdfNode);
    ROOT::RDF::RSnapshotOptions opts;
    opts.fLazy = true;
    std::vector<std::string> columns;
    if (option == TreeOpt::Om::Recreate)
        columns = GetDefinedVarNames();
    else if (option == TreeOpt::Om::Append)
        columns = GetAllVarNames();
    else
        throw std::runtime_error("AnalysisManager: unsupported snapshot option.");
    auto snapshot = m_RdfNode->Snapshot(treeName, fileName, columns, opts);
    m_StartTime = std::chrono::steady_clock::now();
    ROOT::RDF::RunGraphs({callback, snapshot});
    UpdateProgress_(1.0);
}

void AnalysisManager::WriteRdfHistograms(const std::string &outfile)
{
    if (!m_UseRdf) throw std::runtime_error("RDF not initialized");

    std::atomic<ULong64_t> counter = 0;
    const double entryCount = static_cast<double>(GetEntryCount());
    auto callback = m_RdfNode->Count();

    callback.OnPartialResultSlot(500,
                                 [this, &counter, entryCount](unsigned int, ULong64_t &)
                                 {
                                     const auto processed = counter.fetch_add(500) + 500;
                                     UpdateProgress_(double(processed) / entryCount);
                                 });
    // ROOT::RDF::Experimental::AddProgressBar(*m_RdfNode);
    TFile file(outfile.c_str(), "recreate");
    if (file.IsZombie()) throw std::runtime_error("AnalysisManager: cannot create RDF histogram output file: " + outfile);
    file.cd();

    std::vector<ROOT::RDF::RResultHandle> actions{callback};
    for (auto &[_, histograms] : m_HistRdf)
        for (auto &[__, histogram] : histograms)
            actions.emplace_back(histogram);
    m_StartTime = std::chrono::steady_clock::now();
    ROOT::RDF::RunGraphs(actions);
    for (auto &[_, inmap] : m_HistRdf)
    {
        for (auto &[_, hist] : inmap)
            if (hist->Write(hist->GetName(), TObject::kOverwrite) < 0)
                throw std::runtime_error("AnalysisManager: failed to write RDF histogram: " + std::string(hist->GetName()));
    }
    file.Close();
    UpdateProgress_(1.0);
    LOG_INFO("AnalysisManager", "RDF Histograms are saved in " << outfile);
}

std::unique_ptr<AnalysisManager> AnalysisManager::Fork()
{
    if (!m_RdfNode) throw std::runtime_error("AnalysisManager: cannot fork before RDF initialization.");
    auto forked = std::make_unique<AnalysisManager>();
    forked->m_RdfNode = this->m_RdfNode->Filter([]() { return true; });
    forked->m_UseRdf = true;
    forked->m_RawCutExpr = this->m_RawCutExpr;
    forked->m_AppliedRdfCuts = this->m_AppliedRdfCuts;
    forked->m_BranchMap = this->m_BranchMap;
    forked->m_InputFiles = this->m_InputFiles;
    forked->m_InTreeName = this->m_InTreeName;
    forked->m_CurrentTree = this->m_CurrentTree;
    forked->m_CurrentTreeOwner = this->m_CurrentTreeOwner;
    forked->m_LambdaManager = std::make_unique<LambdaManager>();

    return forked;
}

ROOT::RDF::RNode AnalysisManager::GetIsolatedRdfNode()
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

std::ofstream AnalysisManager::OpenOutputFile(const std::string &filename, const std::string &mode) const
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
    const double progress = std::clamp(p, 0.0, 1.0);
    m_Progress.store(progress);
    const long long nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    const long long intervalNs = ProgressReportIntervalNs();
    if (progress < 1.0 && intervalNs > 0)
    {
        long long previous = m_LastProgressReportNs.load(std::memory_order_relaxed);
        if (nowNs - previous < intervalNs ||
            !m_LastProgressReportNs.compare_exchange_strong(previous, nowNs, std::memory_order_relaxed))
            return;
    }
    else
    {
        m_LastProgressReportNs.store(nowNs, std::memory_order_relaxed);
    }
    double elapsed = std::chrono::duration<double>(now - m_StartTime).count();
    double eta = (progress > 0.0 && progress < 1.0) ? elapsed * (1.0 / progress - 1.0) : -1.0;
    Logger::Get().PrintProgressBar("AnalysisManager", progress, elapsed, eta);
}

std::vector<std::string> AnalysisManager::ListInputFiles() const { return m_InputFiles; }

std::map<std::string, std::string> AnalysisManager::ListCutExpressions() const { return m_RawCutExpr; }

std::string AnalysisManager::SnapshotState() const
{
    namespace fs = std::filesystem;
    nlohmann::json inputs = nlohmann::json::array();
    for (const auto &file : m_InputFiles)
    {
        std::error_code error;
        const auto size = fs::file_size(file, error);
        nlohmann::json input = {{"path", file}};
        if (!error)
        {
            const auto modified = fs::last_write_time(file, error);
            input["size"] = size;
            if (!error) input["mtime"] = modified.time_since_epoch().count();
        }
        inputs.push_back(std::move(input));
    }

    nlohmann::json cuts = nlohmann::json::object();
    for (const auto &[name, expression] : m_RawCutExpr)
        cuts[name] = expression;

    nlohmann::json histograms = nlohmann::json::array();
    for (const auto &[alias, prefixes] : m_HistMap)
        for (const auto &[prefix, bins] : prefixes)
        {
            std::string expression;
            if (m_HistExpressions.count(alias) && m_HistExpressions.at(alias).count(prefix))
                expression = m_HistExpressions.at(alias).at(prefix);
            histograms.push_back({{"alias", alias}, {"prefix", prefix}, {"expression", expression}, {"bins", bins}});
        }
    return nlohmann::json{{"schema_version", 2},
                          {"tree", m_InTreeName},
                          {"rdf", m_UseRdf},
                          {"inputs", std::move(inputs)},
                          {"cuts", std::move(cuts)},
                          {"histograms", std::move(histograms)}}
        .dump();
}

void AnalysisManager::WriteMetadata(const std::string &filename, const std::string &hash, const std::string &baseName, const std::string &paramJson)
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
void AnalysisManager::PrintCutSummary()
{
    LOG_INFO("AnalysisManager", "-------------REGISTERED CUTS----------------");
    for (const auto &[name, expr] : m_RawCutExpr)
        LOG_INFO("AnalysisManager", name << " : " << expr);
    LOG_INFO("AnalysisManager", "-------------REGISTERED CUTS----------------");
}

void AnalysisManager::PrintConfigSummary()
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

void AnalysisManager::PrintHistogramSummary()
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
    static std::once_flag loadTreePlayer;
    std::call_once(
        loadTreePlayer,
        []()
        {
            void *handle = dlopen("libTreePlayer.so", RTLD_LAZY | RTLD_GLOBAL);
            if (!handle) LOG_WARN("AnalysisManager", "Failed to load libTreePlayer.so: " << dlerror());
        });
}

void AnalysisManager::ReleaseCurrentTree_()
{
    for (auto &[_, formula] : m_CutFormulas)
        delete formula;
    m_CutFormulas.clear();
    for (auto &[_, formulas] : m_HistFormulas)
        for (auto &[__, formula] : formulas)
            delete formula;
    m_HistFormulas.clear();

    for (auto &[alias, pointer] : m_BranchData)
    {
        const auto info = m_BranchMap.find(alias);
        if (info != m_BranchMap.end()) DeleteBranchPointer(info->second.Type, pointer);
    }
    m_BranchData.clear();
    m_RdfNode.reset();
    m_RdfRaw.reset();
    m_LambdaManager.reset();
    m_UseRdf = false;
    m_AppliedRdfCuts.clear();
    m_CurrentTreeOwner.reset();
    m_CurrentTree = nullptr;
    m_Progress.store(0.0);
}

AnalysisManager::~AnalysisManager()
{
    for (auto &[name, tree] : m_TreeMap)
        if (m_TreeOwnership[name] == ResourceOwnership::Owned) delete tree;
    m_TreeMap.clear();
    m_TreeOwnership.clear();
    ReleaseCurrentTree_();
    m_BranchMap.clear();
    m_NewBranchMap.clear();
    for (auto &[_, ptr] : m_NewBranchData)
        delete ptr;
    m_NewBranchData.clear();
    m_HistMap.clear();
    for (auto &[alias, hists] : m_HistData)
        for (auto &[prefix, h] : hists)
            if (m_HistOwnership[alias][prefix] == ResourceOwnership::Owned) delete h;
    m_HistData.clear();
    m_HistOwnership.clear();
    m_HistExpressions.clear();
    m_LoadedHistMap.clear();
    for (auto &[_, hists] : m_LoadedHistData)
        for (auto &[_, h] : hists)
            delete h;
    m_LoadedHistData.clear();
    m_HistRdf.clear();
    m_RawCutExpr.clear();
    m_InputFiles.clear();
}
