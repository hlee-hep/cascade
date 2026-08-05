#pragma once

#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace logger
{
class Logger;
}

class CancellationToken
{
  public:
    CancellationToken();

    void Request();
    void Reset();
    bool IsCancellationRequested() const;

  private:
    std::shared_ptr<std::atomic<bool>> m_Requested;
};

class OutputTransaction
{
  public:
    OutputTransaction() = default;
    ~OutputTransaction();

    OutputTransaction(const OutputTransaction &) = delete;
    OutputTransaction &operator=(const OutputTransaction &) = delete;

    void Begin(const std::filesystem::path &outputRoot, const std::string &runId);
    std::filesystem::path Stage(const std::filesystem::path &finalPath);
    void Commit();
    void Complete();
    void Rollback() noexcept;
    void CleanupExternalRun() noexcept;

    bool IsActive() const;
    std::filesystem::path OutputRoot() const;
    std::filesystem::path StagingRoot() const;
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> StagedOutputs() const;

  private:
    struct Promotion
    {
        std::filesystem::path Final;
        std::filesystem::path Staged;
        std::filesystem::path Backup;
        bool HadOriginal = false;
    };

    enum class State
    {
        Idle,
        Staging,
        Promoted,
        Completed,
        RolledBack
    };

    mutable std::mutex m_Mutex;
    std::filesystem::path m_OutputRoot;
    std::filesystem::path m_StagingRoot;
    std::map<std::filesystem::path, std::filesystem::path> m_StagedOutputs;
    std::vector<Promotion> m_Promotions;
    State m_State = State::Idle;

    void RollbackUnlocked_() noexcept;
    std::filesystem::path JournalPath_() const;
    void WriteJournal_() const;
    std::vector<Promotion> ReadJournal_() const noexcept;
    static bool IsContained_(const std::filesystem::path &root, const std::filesystem::path &candidate);
};

class ExecutionContext
{
  public:
    ExecutionContext();

    void BeginRun(const std::string &instanceName, const std::string &moduleName);
    void BeginRunWithId(const std::string &instanceName, const std::string &moduleName,
                        const std::string &requestedRunId);
    void CompleteRun();
    void RollbackRun() noexcept;
    void CleanupExternalRun() noexcept;

    void SetCacheDirectory(const std::filesystem::path &path);
    void SetOutputDirectory(const std::filesystem::path &path);

    std::filesystem::path CacheDirectory() const;
    std::filesystem::path OutputDirectory() const;
    std::filesystem::path StageOutput(const std::filesystem::path &path);
    std::filesystem::path FinalOutput(const std::filesystem::path &path) const;
    std::string RunId() const;
    std::string SnapshotState() const;
    bool IsActive() const;

    CancellationToken &Cancellation() { return m_Cancellation; }
    const CancellationToken &Cancellation() const { return m_Cancellation; }
    OutputTransaction &Outputs() { return m_Outputs; }
    const OutputTransaction &Outputs() const { return m_Outputs; }
    logger::Logger &Log() const;

  private:
    mutable std::mutex m_Mutex;
    std::filesystem::path m_CacheDirectory;
    std::filesystem::path m_OutputDirectory;
    std::string m_RunId;
    bool m_Active = false;
    CancellationToken m_Cancellation;
    OutputTransaction m_Outputs;

    static std::filesystem::path DefaultCacheDirectory_();
    static std::filesystem::path DefaultOutputDirectory_();
    static std::string MakeRunId_(const std::string &instanceName, const std::string &moduleName);
};
