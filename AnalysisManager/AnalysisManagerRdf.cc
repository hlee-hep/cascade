#include "AnalysisManager.hh"
#include "AnalysisManagerDetail.inc"
#include "LambdaManager.hh"
#include <ROOT/RDFHelpers.hxx>
#include <TFile.h>
#include <TObject.h>
#include <algorithm>
#include <atomic>

using namespace logger;
using cascade::analysis_detail::SafeColumnName;
using cascade::analysis_detail::ValidateHistogramBins;

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
