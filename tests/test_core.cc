#include "IAnalysisModule.hh"
#include "AMCM.hh"
#include "AnalysisManager.hh"
#include "AnalysisModuleRegistry.hh"
#include "DAGManager.hh"
#include "ParamManager.hh"
#include "PlotManager.hh"

#include <TCanvas.h>
#include <TFile.h>
#include <TH1.h>
#include <TTree.h>
#include <TROOT.h>
#include <cassert>
#include <algorithm>
#include <atomic>
#include <csignal>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
class LifecycleModule final : public IAnalysisModule
{
  public:
    explicit LifecycleModule(ModulePhase failure = ModulePhase::None, std::string baseName = "LifecycleModule") : m_Failure(failure)
    {
        m_Basename = std::move(baseName);
        m_CodeVersionHash = "test";
        m_Param.Set("force_run", true);
    }

    void Description() const override {}
    bool FailureHookCalled = false;

  protected:
    void Init() override
    {
        if (m_Failure == ModulePhase::Init) throw std::runtime_error("init failed");
    }
    void Execute() override
    {
        if (m_Failure == ModulePhase::Execute) throw std::runtime_error("execute failed");
    }
    void Finalize() override
    {
        if (m_Failure == ModulePhase::Finalize) throw std::runtime_error("finalize failed");
    }
    void OnFailure(ModulePhase phase, const std::string &) override { FailureHookCalled = phase == m_Failure; }

  private:
    ModulePhase m_Failure;
};

class TransactionModule final : public IAnalysisModule
{
  public:
    TransactionModule(bool failAfterWrite = false, std::string baseName = "TransactionModule")
        : m_FailAfterWrite(failAfterWrite)
    {
        m_Basename = std::move(baseName);
        m_CodeVersionHash = "test";
        m_Param.Set("force_run", true);
    }

    void Description() const override {}

  protected:
    void Init() override {}
    void Execute() override
    {
        std::ofstream output(StageOutput("result.txt"));
        output << "new";
        output.close();
        if (m_FailAfterWrite) throw std::runtime_error("failure after staged write");
    }
    void Finalize() override {}

  private:
    bool m_FailAfterWrite;
};

class CrashModule final : public IAnalysisModule
{
  public:
    CrashModule()
    {
        m_Basename = "CrashModule";
        m_CodeVersionHash = "test";
        m_Param.Set("force_run", true);
    }
    void Description() const override {}

  protected:
    void Init() override {}
    void Execute() override
    {
        std::ofstream output(StageOutput("crash-result.txt"));
        output << "new";
        output.close();
        Context().Outputs().Commit();
        raise(SIGSEGV);
    }
    void Finalize() override {}
};

void TestLifecycle()
{
    LifecycleModule success;
    const auto successResult = success.Run();
    assert(successResult.Status == ModuleStatus::Done);
    assert(successResult.Phase == ModulePhase::None);

    for (const auto phase : {ModulePhase::Init, ModulePhase::Execute, ModulePhase::Finalize})
    {
        LifecycleModule failure(phase);
        const auto result = failure.Run();
        assert(result.Status == ModuleStatus::Failed);
        assert(result.Phase == phase);
        assert(!result.Message.empty());
        assert(result.HasException());
        assert(failure.FailureHookCalled);
        assert(failure.GetStatusEnum() == ModuleStatus::Failed);
    }

    const auto cacheDirectory = std::filesystem::path(CacheManager::CacheDir());
    std::filesystem::create_directories(cacheDirectory);
    {
        std::ofstream malformed(cacheDirectory / "CheckFailure.yaml");
        malformed << "[invalid";
    }
    LifecycleModule checkFailure(ModulePhase::None, "CheckFailure");
    checkFailure.GetParamManager().Set("force_run", false);
    const auto checkResult = checkFailure.Run();
    assert(checkResult.Status == ModuleStatus::Failed);
    assert(checkResult.Phase == ModulePhase::Check);
    assert(checkResult.HasException());

    const auto invalidCache = std::filesystem::temp_directory_path() / "cascade-cache-file";
    {
        std::ofstream file(invalidCache);
        file << "not a directory";
    }
    LifecycleModule commitFailure(ModulePhase::None, "CommitFailure");
    commitFailure.SetCacheDirectory(invalidCache.string());
    const auto commitResult = commitFailure.Run();
    assert(commitResult.Status == ModuleStatus::Failed);
    assert(commitResult.Phase == ModulePhase::Commit);
    assert(commitResult.HasException());
}

void TestOutputTransactions()
{
    const auto outputDirectory = std::filesystem::temp_directory_path() / "cascade-output-transactions";
    std::filesystem::remove_all(outputDirectory);
    std::filesystem::create_directories(outputDirectory);

    TransactionModule success;
    success.SetOutputDirectory(outputDirectory.string());
    assert(success.Run().Succeeded());
    const auto successProvenance = success.GetLastProvenancePath();
    assert(std::filesystem::is_regular_file(successProvenance));
    {
        std::ifstream input(successProvenance);
        nlohmann::json manifest;
        input >> manifest;
        assert(manifest.at("schema") == "cascade.module-run");
        assert(manifest.at("result").at("status") == "Done");
        assert(manifest.at("identity").at("snapshot_hash").get<std::string>().size() == 64);
        assert(manifest.at("artifacts").at("outputs").size() == 1);
        assert(manifest.at("artifacts").at("outputs").at(0).at("path") == "result.txt");
        assert(manifest.at("artifacts").at("outputs").at(0).at("sha256").get<std::string>().size() == 64);
    }
    {
        std::ifstream output(outputDirectory / "result.txt");
        std::string contents;
        output >> contents;
        assert(contents == "new");
    }

    {
        std::ofstream existing(outputDirectory / "result.txt");
        existing << "old";
    }
    TransactionModule executeFailure(true, "TransactionExecuteFailure");
    executeFailure.SetOutputDirectory(outputDirectory.string());
    const auto executeResult = executeFailure.Run();
    assert(executeResult.Status == ModuleStatus::Failed);
    assert(executeResult.Phase == ModulePhase::Execute);
    assert(std::filesystem::is_regular_file(executeFailure.GetLastProvenancePath()));
    {
        std::ifstream output(outputDirectory / "result.txt");
        std::string contents;
        output >> contents;
        assert(contents == "old");
    }

    const auto invalidCache = outputDirectory / "cache-file";
    {
        std::ofstream file(invalidCache);
        file << "not a directory";
    }
    TransactionModule commitFailure(false, "TransactionCommitFailure");
    commitFailure.SetOutputDirectory(outputDirectory.string());
    commitFailure.SetCacheDirectory(invalidCache.string());
    const auto commitResult = commitFailure.Run();
    assert(commitResult.Status == ModuleStatus::Failed);
    assert(commitResult.Phase == ModulePhase::Commit);
    {
        std::ifstream output(outputDirectory / "result.txt");
        std::string contents;
        output >> contents;
        assert(contents == "old");
    }
    assert(!std::filesystem::exists(outputDirectory / ".cascade-staging"));

    OutputTransaction overlapping;
    overlapping.Begin(outputDirectory, "overlap-test");
    overlapping.Stage("plots");
    bool overlapRejected = false;
    try
    {
        overlapping.Stage("plots/detail.pdf");
    }
    catch (const std::exception &)
    {
        overlapRejected = true;
    }
    assert(overlapRejected);
    overlapping.Rollback();

    const auto outsideDirectory = std::filesystem::temp_directory_path() / "cascade-output-outside";
    std::filesystem::remove_all(outsideDirectory);
    std::filesystem::create_directories(outsideDirectory);
    std::filesystem::create_directory_symlink(outsideDirectory, outputDirectory / "outside-link");
    OutputTransaction escaped;
    escaped.Begin(outputDirectory, "escape-test");
    bool escapeRejected = false;
    try
    {
        escaped.Stage("outside-link/result.txt");
    }
    catch (const std::exception &)
    {
        escapeRejected = true;
    }
    assert(escapeRejected);
    escaped.Rollback();
    std::filesystem::remove(outputDirectory / "outside-link");
    std::filesystem::remove_all(outsideDirectory);
}

void TestProvenanceCacheLink()
{
    const auto root = std::filesystem::temp_directory_path() / "cascade-provenance-cache-link";
    const auto output = root / "output";
    const auto cache = root / "cache";
    std::filesystem::remove_all(root);

    LifecycleModule first(ModulePhase::None, "ProvenanceCacheModule");
    first.SetName("first");
    first.SetOutputDirectory(output.string());
    first.SetCacheDirectory(cache.string());
    first.GetParamManager().Set("force_run", false);
    first.GetParamManager().Register<std::string>("api_token", "do-not-record");
    assert(first.Run().Succeeded());
    const std::string sourceManifest = first.GetLastProvenancePath();

    LifecycleModule second(ModulePhase::None, "ProvenanceCacheModule");
    second.SetName("second");
    second.SetOutputDirectory(output.string());
    second.SetCacheDirectory(cache.string());
    second.GetParamManager().Set("force_run", false);
    second.GetParamManager().Register<std::string>("api_token", "do-not-record");
    const auto skipped = second.Run();
    assert(skipped.Status == ModuleStatus::Skipped);
    std::ifstream input(second.GetLastProvenancePath());
    nlohmann::json manifest;
    input >> manifest;
    assert(manifest.at("execution").at("cache_hit") == true);
    assert(manifest.at("execution").at("cache_source_manifest") == sourceManifest);
    assert(manifest.at("parameters").at("api_token") == "***");
}

void TestControllerContracts()
{
    const std::string className = "CascadeControllerTestModule";
    const auto registeredClasses = AnalysisModuleRegistry::Get().ListModules();
    if (std::find(registeredClasses.begin(), registeredClasses.end(), className) == registeredClasses.end())
        AnalysisModuleRegistry::Get().Register(className, []() { return std::make_unique<LifecycleModule>(); });

    AMCM controller;
    controller.RegisterModule(className, "instance");
    bool duplicateRejected = false;
    try
    {
        controller.RegisterModule(className, "instance");
    }
    catch (const std::exception &)
    {
        duplicateRejected = true;
    }
    assert(duplicateRejected);
    assert(controller.RunAModule("instance").Status == ModuleStatus::Done);
    assert(controller.RunAModuleIsolated("instance").Status == ModuleStatus::Done);
    const auto workflowPath =
        std::filesystem::temp_directory_path() / "cascade-controller-workflow-provenance.json";
    controller.SaveProvenance(workflowPath.string());
    assert(std::filesystem::is_regular_file(workflowPath));
    {
        std::ifstream input(workflowPath);
        nlohmann::json manifest;
        input >> manifest;
        assert(manifest.at("schema") == "cascade.workflow-run");
        assert(!manifest.at("module_manifests").empty());
    }

    const std::string crashClassName = "CascadeControllerCrashModule";
    const auto currentClasses = AnalysisModuleRegistry::Get().ListModules();
    if (std::find(currentClasses.begin(), currentClasses.end(), crashClassName) == currentClasses.end())
        AnalysisModuleRegistry::Get().Register(crashClassName, []() { return std::make_unique<CrashModule>(); });
    const auto crashOutput = std::filesystem::temp_directory_path() / "cascade-isolated-crash";
    std::filesystem::remove_all(crashOutput);
    std::filesystem::create_directories(crashOutput);
    {
        std::ofstream original(crashOutput / "crash-result.txt");
        original << "old";
    }
    auto crash = controller.RegisterModule(crashClassName, "crash");
    crash->SetOutputDirectory(crashOutput.string());
    const auto crashResult = controller.RunAModuleIsolated("crash");
    assert(crashResult.Status == ModuleStatus::Failed);
    assert(crashResult.Phase == ModulePhase::Execute);
    assert(crashResult.Message.find("signal") != std::string::npos);
    {
        std::ifstream restored(crashOutput / "crash-result.txt");
        std::string contents;
        restored >> contents;
        assert(contents == "old");
    }

    bool missingRejected = false;
    try
    {
        controller.GetModule("missing");
    }
    catch (const std::exception &)
    {
        missingRejected = true;
    }
    assert(missingRejected);

    std::atomic<bool> concurrentRegistrationFailed{false};
    std::vector<std::thread> registrationThreads;
    for (int index = 0; index < 4; ++index)
    {
        registrationThreads.emplace_back(
            [&, index]()
            {
                try
                {
                    controller.RegisterModule(className, "parallel_" + std::to_string(index));
                }
                catch (...)
                {
                    concurrentRegistrationFailed.store(true);
                }
            });
    }
    for (auto &thread : registrationThreads)
        thread.join();
    assert(!concurrentRegistrationFailed.load());
    assert(controller.ListRegisteredModules().size() == 6);

    std::atomic<bool> concurrentRunFailed{false};
    std::vector<std::thread> runThreads;
    for (int index = 0; index < 4; ++index)
    {
        runThreads.emplace_back(
            [&, index]()
            {
                const auto result = controller.RunAModule("parallel_" + std::to_string(index));
                if (!result.Succeeded()) concurrentRunFailed.store(true);
            });
    }
    for (auto &thread : runThreads)
        thread.join();
    assert(!concurrentRunFailed.load());
}

void TestParamRoundTrip()
{
    ParamManager source;
    source.Register<int>("count", 3, "event count");
    source.Register<double>("scale", 1.25);
    source.Register<std::string>("mode", "fast");
    source.Register<std::vector<int>>("bins", {1, 2, 3});
    source.Register<std::vector<double>>("empty_weights", {});
    source.Register<MixedVector>("mixed", {1LL, 2.5, std::string("three"), true});
    source.Register<std::monostate>("optional", {});

    const auto temp = std::filesystem::temp_directory_path() / "cascade-param-roundtrip";
    std::filesystem::create_directories(temp);
    const auto yamlPath = temp / "params.yaml";
    const auto jsonPath = temp / "params.json";
    source.SaveYAMLFile(yamlPath.string());
    source.SaveJSONFile(jsonPath.string());

    ParamManager fromYaml;
    fromYaml.Register<int>("count", 0);
    fromYaml.Register<double>("scale", 0.0);
    fromYaml.Register<std::string>("mode", "");
    fromYaml.Register<std::vector<int>>("bins", {});
    fromYaml.Register<std::vector<double>>("empty_weights", {});
    fromYaml.Register<MixedVector>("mixed", {});
    fromYaml.Register<std::monostate>("optional", {});
    fromYaml.LoadYAMLFile(yamlPath.string());
    assert(fromYaml.Get<int>("count") == 3);
    assert(fromYaml.Get<double>("scale") == 1.25);
    assert(fromYaml.Get<std::string>("mode") == "fast");
    assert(fromYaml.Get<std::vector<int>>("bins") == std::vector<int>({1, 2, 3}));
    assert(fromYaml.Get<std::vector<double>>("empty_weights").empty());
    assert(fromYaml.Get<MixedVector>("mixed") == source.Get<MixedVector>("mixed"));
    assert(std::holds_alternative<std::monostate>(fromYaml.Get<ParamValue>("optional")));
    assert(fromYaml.Descriptions().at("count") == "event count");

    ParamManager fromJson;
    fromJson.Register<int>("count", 0);
    fromJson.Register<double>("scale", 0.0);
    fromJson.Register<std::string>("mode", "");
    fromJson.Register<std::vector<int>>("bins", {});
    fromJson.Register<std::vector<double>>("empty_weights", {});
    fromJson.Register<MixedVector>("mixed", {});
    fromJson.Register<std::monostate>("optional", {});
    fromJson.LoadJSONFile(jsonPath.string());
    assert(fromJson.Get<int>("count") == 3);
    assert(fromJson.Get<double>("scale") == 1.25);
    assert(fromJson.Get<std::string>("mode") == "fast");
    assert(fromJson.Get<std::vector<int>>("bins") == std::vector<int>({1, 2, 3}));
    assert(fromJson.Get<std::vector<double>>("empty_weights").empty());
    assert(fromJson.Get<MixedVector>("mixed") == source.Get<MixedVector>("mixed"));
    assert(std::holds_alternative<std::monostate>(fromJson.Get<ParamValue>("optional")));
    assert(fromJson.Descriptions().at("count") == "event count");

    bool rejected = false;
    try
    {
        fromJson.Set("count", std::string("wrong"));
    }
    catch (const std::exception &)
    {
        rejected = true;
    }
    assert(rejected);

    fromJson.Set("count", 4.0);
    assert(fromJson.Get<int>("count") == 4);
    assert(fromJson.TypeOf("count") == "int");
    rejected = false;
    try
    {
        fromJson["count"] = 4.5;
    }
    catch (const std::exception &)
    {
        rejected = true;
    }
    assert(rejected);

    const auto flatJsonPath = temp / "flat.json";
    {
        std::ofstream flatJson(flatJsonPath);
        flatJson << R"({"count": 9})";
    }
    ParamManager flat;
    flat.Register<int>("count", 0);
    flat.LoadJSONFile(flatJsonPath.string());
    assert(flat.Get<int>("count") == 9);
}

void TestAnalysisConfigExpressions()
{
    const auto temp = std::filesystem::temp_directory_path() / "cascade-analysis-config";
    std::filesystem::create_directories(temp);
    const auto inputPath = temp / "input.root";
    const auto inputConfig = temp / "input.yaml";
    const auto histogramConfig = temp / "histograms.yaml";
    const auto histogramOutput = temp / "histograms.root";

    {
        TFile output(inputPath.c_str(), "RECREATE");
        TTree tree("events", "events");
        double raw = 0.0;
        int count = 0;
        bool accepted = false;
        tree.Branch("raw", &raw, "raw/D");
        tree.Branch("count", &count, "count/I");
        tree.Branch("accepted", &accepted, "accepted/O");
        for (const double value : {1.0, 2.0, 3.0})
        {
            raw = value;
            count = static_cast<int>(value);
            accepted = value > 1.0;
            tree.Fill();
        }
        tree.Write();
    }
    {
        std::ofstream output(inputConfig);
        output << "schema_version: 1\n"
                  "input:\n"
                  "  files: ["
               << inputPath.string()
               << "]\n"
                  "  tree: events\n"
                  "branches:\n"
                  "  x:\n"
                  "    name: raw\n"
                  "    type: Double_t\n"
                  "  count:\n"
                  "    name: count\n"
                  "    type: Int_t\n"
                  "  accepted:\n"
                  "    name: accepted\n"
                  "    type: Bool_t\n";
    }
    {
        std::ofstream output(histogramConfig);
        output << "schema_version: 1\n"
                  "histograms:\n"
                  "  doubled:\n"
                  "    expr: x * 2\n"
                  "    bins: [12, 0, 12]\n";
    }

    AnalysisManager manager;
    assert(manager.PreflightInputConfig(inputConfig.string()).Valid());
    manager.LoadInputConfig(inputConfig.string());
    assert(manager.BuildChain());
    manager.RegisterCut("above_one", "x > 1");
    manager.EnableAllCuts();
    assert(manager.PreflightHistogramConfig(histogramConfig.string()).Valid());
    manager.LoadHistogramConfig(histogramConfig.string());
    for (Long64_t index = 0; index < manager.GetEntryCount(); ++index)
    {
        manager.LoadEvent(index);
        assert(manager.GetValue("count") == static_cast<double>(index + 1));
        assert(manager.GetValue("accepted") == (index == 0 ? 0.0 : 1.0));
        if (index == 0)
            assert(!manager.PassesAllCuts());
        else
            assert(manager.PassesAllCuts());
        manager.FillHistograms(1.0);
    }
    manager.WriteHistograms(histogramOutput.string());

    TFile input(histogramOutput.c_str(), "READ");
    auto *histogram = input.Get<TH1>("hist_doubled_");
    assert(histogram);
    assert(histogram->GetEntries() == 3.0);
    assert(std::abs(histogram->GetMean() - 4.0) < 1e-9);

    const auto invalidConfig = temp / "invalid-input.yaml";
    {
        std::ofstream output(invalidConfig);
        output << "schema_version: 99\n"
                  "input:\n"
                  "  files: []\n"
                  "branches: []\n";
    }
    const auto invalidResult = manager.PreflightInputConfig(invalidConfig.string());
    assert(!invalidResult.Valid());
    assert(invalidResult.Errors.size() >= 3);
}

void TestBorrowedRootObjectsRemainAlive()
{
    TTree tree("borrowed_tree", "borrowed_tree");
    TH1D histogram("borrowed_histogram", "borrowed_histogram", 10, 0.0, 1.0);
    histogram.SetDirectory(nullptr);
    {
        AnalysisManager manager;
        manager.RegisterTree(&tree);
        manager.RegisterHistogram("borrowed", &histogram);
        double *derived = manager.RegisterVariable("derived");
        *derived = 1.0;
        assert(!manager.AttachBranch(static_cast<TTree *>(nullptr), "derived", TreeOpt::Om::Append));
        TTree outputTree("output_tree", "output_tree");
        assert(manager.AttachBranch(&outputTree, "derived", TreeOpt::Om::Append));
        assert(outputTree.GetBranch("derived"));
        assert(!manager.AttachBranch(&outputTree, "derived", TreeOpt::Om::Append));
    }
    assert(std::string(tree.GetName()) == "borrowed_tree");
    histogram.Fill(0.5);
    assert(histogram.GetEntries() == 1.0);
}

void TestPlotDoesNotMutateInputs()
{
    gROOT->SetBatch(true);
    const double edges[] = {0.0, 1.0, 3.0, 6.0, 10.0};
    TH1D histogram("plot_input", "plot_input", 4, edges);
    histogram.SetDirectory(nullptr);
    histogram.Fill(0.5, 2.0);
    histogram.Fill(2.0, 3.0);
    const int originalBins = histogram.GetNbinsX();
    const double originalIntegral = histogram.Integral();

    DrawSpec draw;
    draw.SetRebin(2).SetScale(3.0).SetNormBinWidth();
    PlotSpec spec;
    spec.Ratio.Enable = false;
    spec.Stack({StackItemSpec(&histogram, "input", ColorSpec(), draw)});

    bool lumiHookCalled = false;
    PlotManager manager;
    manager.OnLumi([&](TLatex &) { lumiHookCalled = true; });
    TCanvas *canvas = manager.Draw(spec, "plot_immutability_test");
    assert(canvas);
    assert(lumiHookCalled);
    assert(histogram.GetNbinsX() == originalBins);
    assert(histogram.Integral() == originalIntegral);
    delete canvas;

    bool invalidSpecRejected = false;
    try
    {
        PlotManager invalidManager;
        TCanvas *invalidCanvas = invalidManager.Draw(PlotSpec::Simple(), "invalid_plot");
        delete invalidCanvas;
    }
    catch (const std::invalid_argument &)
    {
        invalidSpecRejected = true;
    }
    assert(invalidSpecRejected);

    TH1D numerator("plot_numerator", "plot_numerator", 3, edges);
    TH1D denominator("plot_denominator", "plot_denominator", 3, edges);
    numerator.SetDirectory(nullptr);
    denominator.SetDirectory(nullptr);
    for (const double value : {0.5, 2.0, 4.5})
    {
        numerator.Fill(value, 2.0);
        denominator.Fill(value, 4.0);
    }
    auto numeratorOverlay = OverlaySpec::Hist(&numerator, "data", ColorSpec(), {}, true);
    auto denominatorOverlay = OverlaySpec::Hist(&denominator, "reference", ColorSpec());
    denominatorOverlay.Role = RatioRole::Denominator;
    PlotSpec ratioSpec;
    ratioSpec.UseRatio(true).Overlay({numeratorOverlay, denominatorOverlay});
    PlotManager ratioManager;
    TCanvas *ratioCanvas = ratioManager.Draw(ratioSpec, "ratio_role_test");
    assert(ratioCanvas);
    delete ratioCanvas;

    bool missingDenominatorRejected = false;
    try
    {
        ratioSpec.RatioDenOverlay("missing");
        TCanvas *invalidCanvas = ratioManager.Draw(ratioSpec, "missing_ratio_denominator");
        delete invalidCanvas;
    }
    catch (const std::invalid_argument &)
    {
        missingDenominatorRejected = true;
    }
    assert(missingDenominatorRejected);
}

void TestRdfSnapshotRunsOneEventLoop()
{
    ROOT::DisableImplicitMT();
    const auto temp = std::filesystem::temp_directory_path() / "cascade-rdf-snapshot";
    std::filesystem::create_directories(temp);
    const auto inputPath = temp / "input.root";
    const auto outputPath = temp / "output.root";
    {
        TFile output(inputPath.c_str(), "RECREATE");
        TTree tree("events", "events");
        double raw = 0.0;
        tree.Branch("raw", &raw, "raw/D");
        for (const double value : {1.0, 2.0, 3.0})
        {
            raw = value;
            tree.Fill();
        }
        tree.Write();
    }

    std::atomic<int> evaluations{0};
    AnalysisManager manager;
    manager.InitRdfFromFile("events", inputPath.string());
    manager.DefineRdfVariable(
        "tracked",
        [&](double value)
        {
            ++evaluations;
            return value * 2.0;
        },
        {"raw"});
    manager.WriteRdfSnapshot("events", outputPath.string(), TreeOpt::Om::Recreate);
    assert(evaluations.load() == 3);

    const auto forkOutputPath = temp / "fork-output.root";
    std::unique_ptr<AnalysisManager> forked;
    {
        AnalysisManager parent;
        parent.InitRdfFromFile("events", inputPath.string());
        forked = parent.Fork();
    }
    forked->WriteRdfSnapshot("events", forkOutputPath.string(), TreeOpt::Om::Append);
    TFile forkOutput(forkOutputPath.c_str(), "READ");
    auto *forkTree = forkOutput.Get<TTree>("events");
    assert(forkTree);
    assert(forkTree->GetEntries() == 3);
}

void TestDagValidationAndReset()
{
    ParamManager source;
    ParamManager target;
    source.Register<int>("value", 0);
    target.Register<int>("value", 0);

    int executions = 0;
    bool mutationRejected = false;
    DAGManager dag;
    dag.AddNode("source", {}, [&]() { source.Set("value", 7); });
    dag.AddNode(
        "target", {"source"},
        [&]()
        {
            assert(target.Get<int>("value") == 7);
            try
            {
                dag.AddNode("late", {}, []() {});
            }
            catch (const std::exception &)
            {
                mutationRejected = true;
            }
            ++executions;
        });
    dag.AddDataLink("source", "target", "value -> value",
                    [&]() { target.SetParamVariant("value", source.Get<ParamValue>("value")); });
    const auto firstResult = dag.Execute();
    assert(firstResult.Succeeded());
    assert(executions == 1);
    assert(mutationRejected);
    dag.Reset();
    assert(dag.Execute().Succeeded());
    assert(executions == 2);

    DAGManager invalid;
    invalid.AddNode("node", {"missing"}, []() {});
    bool rejected = false;
    try
    {
        invalid.Execute();
    }
    catch (const std::exception &)
    {
        rejected = true;
    }
    assert(rejected);

    int independentExecutions = 0;
    int downstreamExecutions = 0;
    bool failSource = true;
    DAGManager failure;
    failure.AddNode(
        "source", {},
        [&]()
        {
            if (failSource) throw std::runtime_error("source failed");
        });
    failure.AddNode("downstream", {"source"}, [&]() { ++downstreamExecutions; });
    failure.AddNode("independent", {}, [&]() { ++independentExecutions; });

    const auto failedResult = failure.Execute(false);
    assert(failedResult.Failed());
    assert(independentExecutions == 1);
    assert(downstreamExecutions == 0);
    const auto failedNodes = failure.GetNodeResults();
    const auto statusOf = [&](const std::string &name)
    {
        const auto iterator =
            std::find_if(failedNodes.begin(), failedNodes.end(), [&](const DAGNodeResult &node) { return node.Name == name; });
        assert(iterator != failedNodes.end());
        return iterator->Status;
    };
    assert(statusOf("source") == DAGNodeStatus::Failed);
    assert(statusOf("downstream") == DAGNodeStatus::Blocked);
    assert(statusOf("independent") == DAGNodeStatus::Succeeded);

    failSource = false;
    failure.ResetFailed();
    assert(failure.Execute().Succeeded());
    assert(independentExecutions == 1);
    assert(downstreamExecutions == 1);

    DAGManager invalidLink;
    invalidLink.AddNode("source", {}, []() {});
    invalidLink.AddNode("target", {}, []() {});
    invalidLink.AddDataLink("source", "target", "value", []() {});
    rejected = false;
    try
    {
        invalidLink.Execute();
    }
    catch (const std::exception &)
    {
        rejected = true;
    }
    assert(rejected);

    std::atomic<bool> taskStarted{false};
    std::atomic<bool> taskMayFinish{false};
    DAGManager concurrentMutation;
    concurrentMutation.AddNode(
        "running", {},
        [&]()
        {
            taskStarted.store(true);
            while (!taskMayFinish.load())
                std::this_thread::yield();
        });
    std::thread executor([&]() { concurrentMutation.Execute(); });
    while (!taskStarted.load())
        std::this_thread::yield();
    rejected = false;
    try
    {
        concurrentMutation.AddNode("late", {}, []() {});
    }
    catch (const std::exception &)
    {
        rejected = true;
    }
    taskMayFinish.store(true);
    executor.join();
    assert(rejected);
}
} // namespace

int main()
{
    const auto runtimeRoot = std::filesystem::temp_directory_path() / "cascade-core-test-runtime";
    std::filesystem::remove_all(runtimeRoot);
    const std::string outputRoot = (runtimeRoot / "output").string();
    const std::string cacheRoot = (runtimeRoot / "cache").string();
    setenv("CASCADE_OUTPUT_DIR", outputRoot.c_str(), 1);
    setenv("CASCADE_CACHE_DIR", cacheRoot.c_str(), 1);
    TestLifecycle();
    TestOutputTransactions();
    TestProvenanceCacheLink();
    TestControllerContracts();
    TestParamRoundTrip();
    TestAnalysisConfigExpressions();
    TestBorrowedRootObjectsRemainAlive();
    TestPlotDoesNotMutateInputs();
    TestRdfSnapshotRunsOneEventLoop();
    TestDagValidationAndReset();
    std::filesystem::remove_all(runtimeRoot);
    return 0;
}
