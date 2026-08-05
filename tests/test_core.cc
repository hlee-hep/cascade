#include "IAnalysisModule.hh"
#include "AMCM.hh"
#include "AnalysisManager.hh"
#include "AnalysisModuleRegistry.hh"
#include "CacheManager.hh"
#include "DAGManager.hh"
#include "ParamManager.hh"
#include "PlotManager.hh"
#include "PluginVerifier.hh"
#include "PluginPaths.hh"

#include <TCanvas.h>
#include <TFile.h>
#include <TH1.h>
#include <TTree.h>
#include <TROOT.h>
#include <cassert>
#include <algorithm>
#include <atomic>
#include <csignal>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <thread>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace
{
class LifecycleModule final : public IAnalysisModule
{
  public:
    explicit LifecycleModule(ModulePhase failure = ModulePhase::None, std::string baseName = "LifecycleModule") : m_Failure(failure)
    {
        SetBaseName(std::move(baseName));
        SetCodeHash("test");
        Parameters().Set("force_run", true);
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
        SetBaseName(std::move(baseName));
        SetCodeHash("test");
        Parameters().Set("force_run", true);
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

class TrackedInputModule final : public IAnalysisModule
{
  public:
    TrackedInputModule()
    {
        SetBaseName("TrackedInputModule");
        SetCodeHash("test");
        Parameters().Set("force_run", false);
        Parameters().Register<std::string>("input", "");
    }

    void Description() const override {}
    static std::atomic<int> Executions;

  protected:
    void Init() override { TrackInput(Parameters().Get<std::string>("input")); }
    void Execute() override { Executions.fetch_add(1); }
    void Finalize() override {}
    bool UsesAnalysisManagers() const override { return false; }
};

std::atomic<int> TrackedInputModule::Executions{0};

class SymlinkOutputModule final : public IAnalysisModule
{
  public:
    SymlinkOutputModule()
    {
        SetBaseName("SymlinkOutputModule");
        SetCodeHash("test");
        Parameters().Set("force_run", false);
    }

    void Description() const override {}

  protected:
    void Init() override {}
    void Execute() override
    {
        const auto target = StageOutput("target.txt");
        std::ofstream output(target);
        output << "target";
        output.close();
        std::filesystem::create_symlink("target.txt", StageOutput("link.txt"));
    }
    void Finalize() override {}
    bool UsesAnalysisManagers() const override { return false; }
};

class CrashModule final : public IAnalysisModule
{
  public:
    CrashModule()
    {
        SetBaseName("CrashModule");
        SetCodeHash("test");
        Parameters().Set("force_run", true);
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

class BlockingModule final : public IAnalysisModule
{
  public:
    std::atomic<bool> Started{false};
    std::atomic<bool> MayFinish{false};

    BlockingModule()
    {
        SetBaseName("BlockingModule");
        SetCodeHash("test");
        Parameters().Set("force_run", true);
    }
    void Description() const override {}

  protected:
    bool UsesAnalysisManagers() const override { return false; }
    void Init() override {}
    void Execute() override
    {
        Started.store(true);
        while (!MayFinish.load()) std::this_thread::yield();
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

    BlockingModule blocking;
    std::thread running([&]() { assert(blocking.Run().Succeeded()); });
    while (!blocking.Started.load()) std::this_thread::yield();
    bool liveMutationRejected = false;
    try
    {
        blocking.GetParamManager().Set("force_run", false);
    }
    catch (const std::runtime_error &)
    {
        liveMutationRejected = true;
    }
    assert(liveMutationRejected);
    blocking.MayFinish.store(true);
    running.join();
    blocking.GetParamManager().Set("force_run", false);
}

void TestCacheManagerService()
{
    const auto root = std::filesystem::temp_directory_path() / "cascade-cache-manager-service";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto provenance = root / "run.json";
    {
        std::ofstream output(provenance);
        output << "{}";
    }
    CacheManager::AddHash("first", "present", root.string(), provenance.string());
    CacheManager::AddHash("first", "stale", root.string(), (root / "missing.json").string());
    CacheManager::AddHash("second", "other", root.string(), "");
    assert(CacheManager::IsHashCached("first", "present", root.string()));
    assert(CacheManager::FindProvenance("first", "present", root.string()) == provenance.string());
    assert(CacheManager::ListSnapshots(root.string()).size() == 3);
    assert(CacheManager::ListSnapshots(root.string(), "second").size() == 1);
    assert(CacheManager::Prune(root.string(), "", false, true).size() == 1);
    assert(CacheManager::ListSnapshots(root.string()).size() == 3);
    assert(CacheManager::Prune(root.string(), "", false).size() == 1);
    assert(CacheManager::ListSnapshots(root.string()).size() == 2);
    assert(CacheManager::Prune(root.string(), "first", true).size() == 1);
    assert(CacheManager::ListSnapshots(root.string()).size() == 1);

    setenv("CASCADE_CACHE_MAX_SNAPSHOTS", "2", 1);
    CacheManager::AddHash("bounded", "one", root.string());
    CacheManager::AddHash("bounded", "two", root.string());
    CacheManager::AddHash("bounded", "three", root.string());
    const auto bounded = CacheManager::ListSnapshots(root.string(), "bounded");
    assert(bounded.size() == 2);
    assert(!CacheManager::Lookup("bounded", "one", root.string()));
    assert(CacheManager::Lookup("bounded", "three", root.string()));
    unsetenv("CASCADE_CACHE_MAX_SNAPSHOTS");

    const auto symlinkTarget = root / "outside.yaml";
    {
        std::ofstream output(symlinkTarget);
        output << "schema_version: 1\nsnapshots: []\n";
    }
    std::filesystem::create_symlink(symlinkTarget, root / "linked.yaml");
    bool symlinkRejected = false;
    try
    {
        CacheManager::ListSnapshots(root.string(), "linked");
    }
    catch (const std::exception &)
    {
        symlinkRejected = true;
    }
    assert(symlinkRejected);

    const auto oversizedCache = root / "oversized.yaml";
    {
        std::ofstream output(oversizedCache, std::ios::binary);
        output.seekp(16 * 1024 * 1024);
        output.put('\n');
    }
    bool oversizedCacheRejected = false;
    try
    {
        CacheManager::ListSnapshots(root.string(), "oversized");
    }
    catch (const std::runtime_error &error)
    {
        oversizedCacheRejected = std::string(error.what()).find("16 MiB") != std::string::npos;
    }
    assert(oversizedCacheRejected);
    std::filesystem::remove_all(root);
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

    OutputTransaction firstPublisher;
    OutputTransaction secondPublisher;
    firstPublisher.Begin(outputDirectory, "publisher-one");
    secondPublisher.Begin(outputDirectory, "publisher-two");
    const auto firstStaged = firstPublisher.Stage("shared.txt");
    const auto secondStaged = secondPublisher.Stage("shared.txt");
    {
        std::ofstream output(firstStaged);
        output << "first";
    }
    {
        std::ofstream output(secondStaged);
        output << "second";
    }
    firstPublisher.Commit();
    std::atomic<bool> secondCommitStarted{false};
    std::atomic<bool> secondCommitFinished{false};
    std::thread secondCommit(
        [&]()
        {
            secondCommitStarted.store(true);
            secondPublisher.Commit();
            secondCommitFinished.store(true);
        });
    while (!secondCommitStarted.load()) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    assert(!secondCommitFinished.load());
    firstPublisher.Complete();
    secondCommit.join();
    secondPublisher.Complete();
    {
        std::ifstream output(outputDirectory / "shared.txt");
        std::string contents;
        output >> contents;
        assert(contents == "second");
    }

    OutputTransaction directoryPublisher;
    OutputTransaction childPublisher;
    directoryPublisher.Begin(outputDirectory, "directory-publisher");
    childPublisher.Begin(outputDirectory, "child-publisher");
    const auto directoryStaged = directoryPublisher.Stage("shared-directory");
    const auto childStaged = childPublisher.Stage("shared-directory/item.txt");
    std::filesystem::create_directories(directoryStaged);
    {
        std::ofstream output(directoryStaged / "initial.txt");
        output << "directory";
    }
    {
        std::ofstream output(childStaged);
        output << "child";
    }
    directoryPublisher.Commit();
    std::atomic<bool> childCommitStarted{false};
    std::atomic<bool> childCommitFinished{false};
    std::thread childCommit(
        [&]()
        {
            childCommitStarted.store(true);
            childPublisher.Commit();
            childCommitFinished.store(true);
        });
    while (!childCommitStarted.load()) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    assert(!childCommitFinished.load());
    directoryPublisher.Complete();
    childCommit.join();
    childPublisher.Complete();
    assert(std::filesystem::is_regular_file(outputDirectory / "shared-directory" / "initial.txt"));
    assert(std::filesystem::is_regular_file(outputDirectory / "shared-directory" / "item.txt"));

    OutputTransaction crashedPublisher;
    crashedPublisher.Begin(outputDirectory, "crashed-publisher");
    const auto crashedStaged = crashedPublisher.Stage("recovered.txt");
    {
        std::ofstream output(crashedStaged);
        output << "crashed";
    }
    const pid_t publisherChild = fork();
    assert(publisherChild >= 0);
    if (publisherChild == 0)
    {
        crashedPublisher.Commit();
        _exit(0);
    }
    int publisherStatus = 0;
    assert(waitpid(publisherChild, &publisherStatus, 0) == publisherChild);
    assert(WIFEXITED(publisherStatus) && WEXITSTATUS(publisherStatus) == 0);

    OutputTransaction newerPublisher;
    newerPublisher.Begin(outputDirectory, "newer-publisher");
    const auto newerStaged = newerPublisher.Stage("recovered.txt");
    {
        std::ofstream output(newerStaged);
        output << "newer";
    }
    newerPublisher.Commit();
    newerPublisher.Complete();
    crashedPublisher.CleanupExternalRun();
    {
        std::ifstream output(outputDirectory / "recovered.txt");
        std::string contents;
        output >> contents;
        assert(contents == "newer");
    }

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
    assert(skipped.CacheDecision == "hit");
    assert(skipped.CacheReason.find("outputs matched") != std::string::npos);
    std::ifstream input(second.GetLastProvenancePath());
    nlohmann::json manifest;
    input >> manifest;
    assert(manifest.at("execution").at("cache_hit") == true);
    assert(manifest.at("execution").at("cache_source_manifest") == sourceManifest);
    assert(manifest.at("parameters").at("api_token") == "***");
}

void TestCacheIntegrityValidation()
{
    const char *configuredInputHashMode = std::getenv("CASCADE_INPUT_HASH_MODE");
    const bool hadConfiguredInputHashMode = configuredInputHashMode && *configuredInputHashMode;
    const std::string originalInputHashMode = hadConfiguredInputHashMode ? configuredInputHashMode : "";
    unsetenv("CASCADE_INPUT_HASH_MODE");
    const auto root = std::filesystem::temp_directory_path() / "cascade-cache-integrity";
    const auto output = root / "output";
    const auto cache = root / "cache";
    const auto inputPath = root / "input.txt";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    {
        std::ofstream input(inputPath);
        input << "first";
    }
    const auto originalTime = std::filesystem::last_write_time(inputPath);
    TrackedInputModule::Executions.store(0);

    TrackedInputModule first;
    first.SetName("tracked-first");
    first.SetOutputDirectory(output.string());
    first.SetCacheDirectory(cache.string());
    first.GetParamManager().Set("input", inputPath.string());
    assert(first.Run().Status == ModuleStatus::Done);
    {
        std::ifstream input(first.GetLastProvenancePath());
        nlohmann::json manifest;
        input >> manifest;
        const auto &tracked = manifest.at("artifacts").at("inputs").at(0);
        assert(tracked.at("sha256").is_null());
        assert(tracked.at("identity").at("inode").get<std::uintmax_t>() != 0);
    }

    TrackedInputModule cached;
    cached.SetName("tracked-cached");
    cached.SetOutputDirectory(output.string());
    cached.SetCacheDirectory(cache.string());
    cached.GetParamManager().Set("input", inputPath.string());
    assert(cached.Run().Status == ModuleStatus::Skipped);
    assert(TrackedInputModule::Executions.load() == 1);

    {
        std::ofstream input(inputPath, std::ios::trunc);
        input << "other";
    }
    std::filesystem::last_write_time(inputPath, originalTime);
    TrackedInputModule changed;
    changed.SetName("tracked-changed");
    changed.SetOutputDirectory(output.string());
    changed.SetCacheDirectory(cache.string());
    changed.GetParamManager().Set("input", inputPath.string());
    assert(changed.Run().Status == ModuleStatus::Done);
    assert(TrackedInputModule::Executions.load() == 2);

    setenv("CASCADE_INPUT_HASH_MODE", "full", 1);
    TrackedInputModule strict;
    strict.SetName("tracked-strict");
    strict.SetOutputDirectory(output.string());
    strict.SetCacheDirectory(cache.string());
    strict.GetParamManager().Set("input", inputPath.string());
    assert(strict.Run().Status == ModuleStatus::Done);
    assert(TrackedInputModule::Executions.load() == 3);
    {
        std::ifstream input(strict.GetLastProvenancePath());
        nlohmann::json manifest;
        input >> manifest;
        assert(manifest.at("artifacts").at("inputs").at(0).at("sha256").get<std::string>().size() == 64);
    }
    if (hadConfiguredInputHashMode)
        setenv("CASCADE_INPUT_HASH_MODE", originalInputHashMode.c_str(), 1);
    else
        unsetenv("CASCADE_INPUT_HASH_MODE");

    TransactionModule outputFirst(false, "OutputIntegrityModule");
    outputFirst.SetName("output-first");
    outputFirst.SetOutputDirectory(output.string());
    outputFirst.SetCacheDirectory(cache.string());
    outputFirst.GetParamManager().Set("force_run", false);
    assert(outputFirst.Run().Status == ModuleStatus::Done);
    {
        std::ofstream corrupted(output / "result.txt", std::ios::trunc);
        corrupted << "bad";
    }
    TransactionModule outputChanged(false, "OutputIntegrityModule");
    outputChanged.SetName("output-changed");
    outputChanged.SetOutputDirectory(output.string());
    outputChanged.SetCacheDirectory(cache.string());
    outputChanged.GetParamManager().Set("force_run", false);
    const auto changedOutputResult = outputChanged.Run();
    assert(changedOutputResult.Status == ModuleStatus::Done);
    assert(changedOutputResult.CacheDecision == "miss");
    assert(changedOutputResult.CacheReason.find("content changed") != std::string::npos);
    {
        std::ifstream restored(output / "result.txt");
        std::string contents;
        restored >> contents;
        assert(contents == "new");
    }

    const char *configuredOutputHashMode = std::getenv("CASCADE_PROVENANCE_HASH_MODE");
    const bool hadConfiguredOutputHashMode = configuredOutputHashMode && *configuredOutputHashMode;
    const std::string originalOutputHashMode = hadConfiguredOutputHashMode ? configuredOutputHashMode : "";
    setenv("CASCADE_PROVENANCE_HASH_MODE", "none", 1);
    TransactionModule metadataFirst(false, "MetadataOutputModule");
    metadataFirst.SetName("metadata-first");
    metadataFirst.SetOutputDirectory(output.string());
    metadataFirst.SetCacheDirectory(cache.string());
    metadataFirst.GetParamManager().Set("force_run", false);
    assert(metadataFirst.Run().Status == ModuleStatus::Done);
    assert(chmod((output / "result.txt").c_str(), 0000) == 0);
    TransactionModule metadataCached(false, "MetadataOutputModule");
    metadataCached.SetName("metadata-cached");
    metadataCached.SetOutputDirectory(output.string());
    metadataCached.SetCacheDirectory(cache.string());
    metadataCached.GetParamManager().Set("force_run", false);
    const auto metadataResult = metadataCached.Run();
    assert(metadataResult.Status == ModuleStatus::Skipped);
    assert(metadataResult.CacheDecision == "hit");
    assert(chmod((output / "result.txt").c_str(), 0600) == 0);

    setenv("CASCADE_PROVENANCE_HASH_MODE", "full", 1);
    SymlinkOutputModule symlinkFirst;
    symlinkFirst.SetName("symlink-first");
    symlinkFirst.SetOutputDirectory(output.string());
    symlinkFirst.SetCacheDirectory(cache.string());
    assert(symlinkFirst.Run().Status == ModuleStatus::Done);
    assert(std::filesystem::is_symlink(output / "link.txt"));
    SymlinkOutputModule symlinkCached;
    symlinkCached.SetName("symlink-cached");
    symlinkCached.SetOutputDirectory(output.string());
    symlinkCached.SetCacheDirectory(cache.string());
    assert(symlinkCached.Run().Status == ModuleStatus::Skipped);
    if (hadConfiguredOutputHashMode)
        setenv("CASCADE_PROVENANCE_HASH_MODE", originalOutputHashMode.c_str(), 1);
    else
        unsetenv("CASCADE_PROVENANCE_HASH_MODE");
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
    bool unverifiedIsolationRejected = false;
    try
    {
        controller.RunAModuleIsolated("instance");
    }
    catch (const std::runtime_error &error)
    {
        unverifiedIsolationRejected = std::string(error.what()).find("verified plugin") != std::string::npos;
    }
    assert(unverifiedIsolationRejected);

    const auto isolatedOutput = std::filesystem::temp_directory_path() / "cascade-exec-worker";
    const auto isolatedCache = std::filesystem::temp_directory_path() / "cascade-exec-worker-cache";
    std::filesystem::remove_all(isolatedOutput);
    std::filesystem::remove_all(isolatedCache);
    auto workerModule = controller.RegisterModule("WorkerTestModule", "worker-instance");
    workerModule->SetOutputDirectory(isolatedOutput.string());
    workerModule->SetCacheDirectory(isolatedCache.string());
    const auto workerResult = controller.RunAModuleIsolated(workerModule);
    assert(workerResult.Succeeded());
    assert(workerResult.CacheDecision == "bypassed");
    {
        std::ifstream output(isolatedOutput / "worker-result.txt");
        std::string contents;
        output >> contents;
        assert(contents == "exec-worker");
    }

    const char *configuredWorker = std::getenv("CASCADE_CPP_WORKER");
    assert(configuredWorker && *configuredWorker);
    const std::string originalWorker(configuredWorker);
    auto hardenedModule = controller.RegisterModule("WorkerTestModule", "hardened-worker-instance");
    hardenedModule->SetOutputDirectory(isolatedOutput.string());
    hardenedModule->SetCacheDirectory(isolatedCache.string());
    setenv("CASCADE_CPP_WORKER", "relative-worker", 1);
    bool relativeWorkerRejected = false;
    try
    {
        controller.RunAModuleIsolated(hardenedModule);
    }
    catch (const std::runtime_error &error)
    {
        relativeWorkerRejected = std::string(error.what()).find("absolute path") != std::string::npos;
    }
    assert(relativeWorkerRejected);

    const auto workerSecurityRoot = std::filesystem::current_path() / "build" / "test-worker-security";
    std::filesystem::remove_all(workerSecurityRoot);
    std::filesystem::create_directories(workerSecurityRoot);
    const auto customWorker = workerSecurityRoot / "untrusted-worker";
    const auto leakedEnvironment = isolatedOutput / "worker-environment-leaked";
    {
        std::ofstream script(customWorker);
        script << "#!/usr/bin/python3\n"
               << "import os, time\n"
               << "if 'LD_PRELOAD' in os.environ:\n"
               << "    open('" << leakedEnvironment.string() << "', 'w').close()\n"
               << "if os.fork() == 0:\n"
               << "    time.sleep(1)\n"
               << "    os._exit(0)\n"
               << "os._exit(0)\n";
    }
    assert(chmod(customWorker.c_str(), 0777) == 0);
    setenv("CASCADE_CPP_WORKER", customWorker.string().c_str(), 1);
    bool writableWorkerRejected = false;
    try
    {
        controller.RunAModuleIsolated(hardenedModule);
    }
    catch (const std::runtime_error &error)
    {
        writableWorkerRejected = std::string(error.what()).find("writable") != std::string::npos;
    }
    assert(writableWorkerRejected);

    const auto writableParent = workerSecurityRoot / "writable-parent";
    std::filesystem::create_directories(writableParent);
    assert(chmod(writableParent.c_str(), 0777) == 0);
    const auto parentWorker = writableParent / "worker";
    std::filesystem::copy_file(customWorker, parentWorker);
    assert(chmod(parentWorker.c_str(), 0700) == 0);
    setenv("CASCADE_CPP_WORKER", parentWorker.string().c_str(), 1);
    bool writableParentRejected = false;
    try
    {
        controller.RunAModuleIsolated(hardenedModule);
    }
    catch (const std::runtime_error &error)
    {
        writableParentRejected = std::string(error.what()).find("parent directory") != std::string::npos;
    }
    assert(writableParentRejected);

    assert(chmod(customWorker.c_str(), 0700) == 0);
    setenv("CASCADE_CPP_WORKER", customWorker.string().c_str(), 1);
    setenv("LD_PRELOAD", "/cascade/nonexistent-preload.so", 1);
    const auto workerStarted = std::chrono::steady_clock::now();
    const auto invalidWorkerResult = controller.RunAModuleIsolated(hardenedModule);
    const auto workerElapsed = std::chrono::steady_clock::now() - workerStarted;
    unsetenv("LD_PRELOAD");
    setenv("CASCADE_CPP_WORKER", originalWorker.c_str(), 1);
    assert(invalidWorkerResult.Status == ModuleStatus::Failed);
    assert(workerElapsed < std::chrono::milliseconds(750));
    assert(!std::filesystem::exists(leakedEnvironment));
    assert(controller.RefreshPlugins().empty());

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
    assert(controller.ListRegisteredModules().size() == 7);

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

    ParamManager concurrent;
    concurrent.Register<int>("value", 0);
    std::atomic<bool> paramFailure{false};
    std::vector<std::thread> paramThreads;
    for (int threadIndex = 0; threadIndex < 8; ++threadIndex)
        paramThreads.emplace_back(
            [&, threadIndex]()
            {
                try
                {
                    for (int iteration = 0; iteration < 2000; ++iteration)
                    {
                        concurrent.Set("value", threadIndex * 2000 + iteration);
                        (void)concurrent.Get<int>("value");
                        if ((iteration % 100) == 0) (void)concurrent.DumpJSON();
                    }
                }
                catch (...)
                {
                    paramFailure.store(true);
                }
            });
    for (auto &thread : paramThreads)
        thread.join();
    assert(!paramFailure.load());
    assert(concurrent.TypeOf("value") == "int");

    ParamManager copied = concurrent;
    ParamManager moved = std::move(copied);
    assert(moved.TypeOf("value") == "int");
    assert(moved.Get<int>("value") >= 0);
    moved.Set("value", 1);
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
    dag.Validate();
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
        invalid.Validate();
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

void TestDagExecutionLanes()
{
    setenv("CASCADE_DAG_MAX_WORKERS", "4", 1);
    std::atomic<int> entered{0};
    std::atomic<bool> overlapped{false};
    DAGManager parallel;
    auto concurrentTask = [&]()
    {
        if (entered.fetch_add(1) + 1 == 2) overlapped.store(true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (entered.load() < 2 && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    };
    parallel.AddNode("left", {}, concurrentTask, DAGExecutionLane::Parallel);
    parallel.AddNode("right", {}, concurrentTask, DAGExecutionLane::Parallel);
    assert(parallel.Execute().Succeeded());
    assert(overlapped.load());

    std::atomic<bool> slowFinished{false};
    std::atomic<bool> dependentStartedBeforeSlowFinished{false};
    DAGManager eventDriven;
    eventDriven.AddNode(
        "fast", {}, []() { std::this_thread::sleep_for(std::chrono::milliseconds(20)); },
        DAGExecutionLane::Parallel);
    eventDriven.AddNode(
        "slow", {},
        [&]()
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            slowFinished.store(true);
        },
        DAGExecutionLane::Parallel);
    eventDriven.AddNode(
        "dependent", {"fast"},
        [&]() { dependentStartedBeforeSlowFinished.store(!slowFinished.load()); },
        DAGExecutionLane::Parallel);
    assert(eventDriven.Execute().Succeeded());
    assert(dependentStartedBeforeSlowFinished.load());

    std::atomic<int> activeRoots{0};
    std::atomic<int> maximumRoots{0};
    DAGManager roots;
    auto rootTask = [&]()
    {
        const int active = activeRoots.fetch_add(1) + 1;
        int observed = maximumRoots.load();
        while (active > observed && !maximumRoots.compare_exchange_weak(observed, active))
        {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        activeRoots.fetch_sub(1);
    };
    roots.AddNode("root-left", {}, rootTask, DAGExecutionLane::Root);
    roots.AddNode("root-right", {}, rootTask, DAGExecutionLane::Root);
    assert(roots.Execute().Succeeded());
    assert(maximumRoots.load() == 1);
    unsetenv("CASCADE_DAG_MAX_WORKERS");
}

void TestPluginTrustPolicy()
{
    const std::string className = "CascadeVerifiedPolicyModule";
    auto &registry = AnalysisModuleRegistry::Get();
    registry.Register(className, []() { return std::make_unique<LifecycleModule>(); });
    PluginOrigin verifiedOrigin;
    verifiedOrigin.Package = "local-package";
    verifiedOrigin.ManifestPath = "/tmp/local-package/plugin_manifest.json";
    verifiedOrigin.ManifestSha256 = std::string(64, 'a');
    verifiedOrigin.ArtifactSha256 = std::string(64, 'b');
    verifiedOrigin.Trust = PluginTrustStatus::Verified;
    registry.SetPluginOrigin(className, verifiedOrigin);

    AMCM verifiedController;
    const auto verifiedModules = verifiedController.ListAvailableModules();
    assert(std::find(verifiedModules.begin(), verifiedModules.end(), className) != verifiedModules.end());
    auto module = verifiedController.RegisterModule(className, "verified-policy-instance");
    const auto output = std::filesystem::temp_directory_path() / "cascade-verified-policy";
    std::filesystem::remove_all(output);
    module->SetOutputDirectory(output.string());
    assert(verifiedController.RunAModule(module).Succeeded());
    {
        std::ifstream input(module->GetLastProvenancePath());
        nlohmann::json manifest;
        input >> manifest;
        assert(manifest.at("plugin").at("package") == "local-package");
        assert(manifest.at("plugin").at("trust") == "Verified");
        assert(manifest.at("plugin").at("signer_fingerprint").is_null());
    }

    AMCM strictController(PluginTrustPolicy::RequireSigned);
    const auto strictModules = strictController.ListAvailableModules();
    assert(std::find(strictModules.begin(), strictModules.end(), className) == strictModules.end());
    bool unsignedRejected = false;
    try
    {
        strictController.RegisterModule(className, "strict-rejected");
    }
    catch (const std::exception &)
    {
        unsignedRejected = true;
    }
    assert(unsignedRejected);

    verifiedOrigin.Trust = PluginTrustStatus::Signed;
    verifiedOrigin.SignerFingerprint = std::string(64, 'c');
    registry.SetPluginOrigin(className, verifiedOrigin);
    const auto signedModules = strictController.ListAvailableModules();
    assert(std::find(signedModules.begin(), signedModules.end(), className) != signedModules.end());
    strictController.RegisterModule(className, "strict-signed");
    registry.Unregister(className);
    std::filesystem::remove_all(output);
}

void TestPluginVerifierService()
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "cascade-plugin-verifier-service";
    const fs::path package = root / "pyplugin" / "example-package";
    const fs::path trustStore = root / "trusted_keys";
    fs::remove_all(root);
    fs::create_directories(package);
    fs::create_directories(trustStore);
    {
        std::ofstream source(package / "example_module.py");
        source << "hello";
    }
    nlohmann::json manifest = {
        {"schema", 2},
        {"package", "example-package"},
        {"modules",
         {{{"name", "example_module"},
           {"language", "python"},
           {"path", "example_module.py"},
           {"sha256", "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824"},
           {"classes", {"ExampleModule"}}}}},
    };
    {
        std::ofstream output(package / "plugin_manifest.json");
        output << manifest.dump(2) << '\n';
    }

    const auto verified = PluginVerifier::VerifyPackage(package.string(), trustStore.string(),
                                                        PluginTrustPolicy::Verified, "python");
    assert(verified.Package == "example-package");
    assert(verified.Trust == PluginTrustStatus::Verified);
    assert(verified.Artifacts.size() == 1);
    assert(verified.Artifacts.front().Source == "hello");
    assert(verified.Artifacts.front().Classes == std::vector<std::string>{"ExampleModule"});
    assert(PluginVerifier::HashFile((package / "example_module.py").string()) ==
           "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
    assert(PluginVerifier::ReadFile((package / "example_module.py").string()) == "hello");
    PluginVerifier::ValidateStagedTree(package.string());
    const auto discovery = PluginVerifier::Discover({(root / "pyplugin").string()},
                                                     PluginTrustPolicy::Verified, "python");
    assert(discovery.Errors.empty());
    assert(discovery.Packages.size() == 1);

    assert(chmod(package.c_str(), 0777) == 0);
    bool writableUnsignedPackageRejected = false;
    try
    {
        PluginVerifier::VerifyPackage(package.string(), trustStore.string(), PluginTrustPolicy::Verified,
                                      "python");
    }
    catch (const std::runtime_error &error)
    {
        writableUnsignedPackageRejected = std::string(error.what()).find("writable") != std::string::npos;
    }
    assert(chmod(package.c_str(), 0755) == 0);
    assert(writableUnsignedPackageRejected);

    {
        std::ofstream output(package / "unused.py");
        output << "unused";
    }
    manifest["modules"].push_back({{"name", "unused_module"},
                                   {"language", "python"},
                                   {"path", "unused.py"},
                                   {"sha256", std::string(64, '0')},
                                   {"classes", {"UnusedModule"}}});
    {
        std::ofstream output(package / "plugin_manifest.json");
        output << manifest.dump(2) << '\n';
    }
    const auto targeted = PluginVerifier::VerifyPackage(package.string(), trustStore.string(),
                                                        PluginTrustPolicy::Verified, "python", "", "ExampleModule");
    assert(targeted.Artifacts.size() == 1);
    assert(targeted.Artifacts.front().Classes == std::vector<std::string>{"ExampleModule"});
    manifest["modules"].erase(manifest["modules"].end() - 1);
    {
        std::ofstream output(package / "plugin_manifest.json");
        output << manifest.dump(2) << '\n';
    }

    const fs::path prefix = root / "prefix";
    fs::create_directories(prefix);
    const fs::path config = root / "config.json";
    {
        std::ofstream output(config);
        nlohmann::json configDocument = {
            {"schema", 1},
            {"plugin_prefixes", nlohmann::json::array({{{"path", prefix.string()}, {"enabled", true}}})},
        };
        output << configDocument.dump();
    }
    setenv("CASCADE_CONFIG_FILE", config.string().c_str(), 1);
    const auto prefixes = PluginPaths::ConfiguredPrefixes();
    assert(prefixes == std::vector<std::string>{fs::weakly_canonical(prefix).string()});
    assert(PluginPaths::Layout(prefix.string()).Python == (prefix / "lib" / "cascade" / "pyplugin").string());
    unsetenv("CASCADE_CONFIG_FILE");

    bool strictRejected = false;
    try
    {
        PluginVerifier::VerifyPackage(package.string(), trustStore.string(), PluginTrustPolicy::RequireSigned,
                                      "python");
    }
    catch (const std::runtime_error &)
    {
        strictRejected = true;
    }
    assert(strictRejected);

    manifest["modules"][0]["sha256"] = std::string(64, '0');
    {
        std::ofstream output(package / "plugin_manifest.json");
        output << manifest.dump(2) << '\n';
    }
    bool hashRejected = false;
    try
    {
        PluginVerifier::VerifyPackage(package.string(), trustStore.string());
    }
    catch (const std::runtime_error &)
    {
        hashRejected = true;
    }
    assert(hashRejected);
    fs::remove_all(root);
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
    TestCacheManagerService();
    TestOutputTransactions();
    TestProvenanceCacheLink();
    TestCacheIntegrityValidation();
    TestControllerContracts();
    TestPluginTrustPolicy();
    TestPluginVerifierService();
    TestParamRoundTrip();
    TestAnalysisConfigExpressions();
    TestBorrowedRootObjectsRemainAlive();
    TestPlotDoesNotMutateInputs();
    TestRdfSnapshotRunsOneEventLoop();
    TestDagValidationAndReset();
    TestDagExecutionLanes();
    std::filesystem::remove_all(runtimeRoot);
    return 0;
}
