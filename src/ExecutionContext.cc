#include "ExecutionContext.hh"

#include "InterruptManager.hh"
#include "Logger.hh"
#include "sha256.hh"

#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace
{
std::atomic<unsigned long long> g_RunCounter{0};

std::string SafeName(std::string value)
{
    for (char &character : value)
        if (!std::isalnum(static_cast<unsigned char>(character)) && character != '-' && character != '_') character = '_';
    return value.empty() ? "unnamed" : value;
}

fs::path AbsoluteNormalized(const fs::path &path)
{
    return fs::weakly_canonical(fs::absolute(path));
}
} // namespace

CancellationToken::CancellationToken() : m_Requested(std::make_shared<std::atomic<bool>>(false)) {}

void CancellationToken::Request() { m_Requested->store(true); }

void CancellationToken::Reset() { m_Requested->store(false); }

bool CancellationToken::IsCancellationRequested() const
{
    return m_Requested->load() || InterruptManager::IsInterrupted();
}

OutputTransaction::~OutputTransaction() { Rollback(); }

void OutputTransaction::Begin(const fs::path &outputRoot, const std::string &runId)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    RollbackUnlocked_();
    m_OutputRoot = AbsoluteNormalized(outputRoot);
    m_StagingRoot = m_OutputRoot / ".cascade-staging" / runId;
    m_StagedOutputs.clear();
    m_Promotions.clear();
    m_State = State::Staging;
}

bool OutputTransaction::IsContained_(const fs::path &root, const fs::path &candidate)
{
    if (candidate == root) return false;
    const fs::path relative = candidate.lexically_relative(root);
    return !relative.empty() && *relative.begin() != "..";
}

fs::path OutputTransaction::Stage(const fs::path &finalPath)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_State != State::Staging) throw std::runtime_error("OutputTransaction: no active staging transaction.");

    const fs::path final = finalPath.is_absolute() ? AbsoluteNormalized(finalPath) : AbsoluteNormalized(m_OutputRoot / finalPath);
    if (!IsContained_(m_OutputRoot, final) || final == m_OutputRoot)
        throw std::runtime_error("OutputTransaction: output path must be inside the configured output directory: " + final.string());

    const fs::path relative = final.lexically_relative(m_OutputRoot);
    const fs::path staged = m_StagingRoot / "files" / relative;
    fs::create_directories(staged.parent_path());
    for (const auto &[existing, _] : m_StagedOutputs)
    {
        if (existing == final) continue;
        if (IsContained_(existing, final) || IsContained_(final, existing))
            throw std::runtime_error("OutputTransaction: staged outputs cannot overlap: " + existing.string() + " and " + final.string());
    }
    auto [iterator, inserted] = m_StagedOutputs.emplace(final, staged);
    if (!inserted && iterator->second != staged)
        throw std::runtime_error("OutputTransaction: conflicting staged output path: " + final.string());
    return staged;
}

void OutputTransaction::AcquireOutputLocks_(const std::vector<fs::path> &finalPaths)
{
    if (!m_OutputLockDescriptors.empty()) throw std::logic_error("OutputTransaction: output locks are already held.");
    const fs::path lockDirectory = m_OutputRoot / ".cascade" / "locks" / "outputs";
    fs::create_directories(lockDirectory);
    std::map<fs::path, int> requests;
    for (const auto &final : finalPaths)
    {
        fs::path current = m_OutputRoot;
        const fs::path relative = final.lexically_relative(m_OutputRoot);
        if (relative.empty() || *relative.begin() == "..")
            throw std::runtime_error("OutputTransaction: cannot lock an output outside the configured root.");
        for (const auto &component : relative)
        {
            current /= component;
            const int operation = current == final ? LOCK_EX : LOCK_SH;
            auto [iterator, inserted] = requests.emplace(current, operation);
            if (!inserted && operation == LOCK_EX) iterator->second = LOCK_EX;
        }
    }
    try
    {
        for (const auto &[path, operation] : requests)
        {
            const fs::path lockPath = lockDirectory / (Sha256(path.string()) + ".lock");
            int flags = O_CREAT | O_RDWR;
#ifdef O_CLOEXEC
            flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
            flags |= O_NOFOLLOW;
#endif
            const int descriptor = open(lockPath.c_str(), flags, 0600);
            if (descriptor < 0)
                throw std::system_error(errno, std::generic_category(), "OutputTransaction: cannot open output lock");
            struct stat metadata{};
            if (fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode))
            {
                const int error = errno ? errno : EINVAL;
                close(descriptor);
                throw std::system_error(error, std::generic_category(), "OutputTransaction: invalid output lock");
            }
            while (flock(descriptor, operation) != 0)
            {
                if (errno == EINTR) continue;
                const int error = errno;
                close(descriptor);
                throw std::system_error(error, std::generic_category(), "OutputTransaction: cannot lock output");
            }
            m_OutputLockDescriptors.push_back(descriptor);
        }
    }
    catch (...)
    {
        ReleaseOutputLocks_();
        throw;
    }
}

bool OutputTransaction::AcquireRecoveryLocks_(const std::vector<Promotion> &promotions) noexcept
{
    if (!m_OutputLockDescriptors.empty() || promotions.empty()) return true;
    std::vector<fs::path> finalPaths;
    finalPaths.reserve(promotions.size());
    for (const auto &promotion : promotions)
        finalPaths.push_back(promotion.Final);
    try
    {
        AcquireOutputLocks_(finalPaths);
        return true;
    }
    catch (const std::exception &error)
    {
        LOG_ERROR("OutputTransaction", "Cannot acquire recovery locks: " << error.what());
    }
    catch (...)
    {
        LOG_ERROR("OutputTransaction", "Cannot acquire recovery locks due to an unknown error");
    }
    return false;
}

void OutputTransaction::ReleaseOutputLocks_() noexcept
{
    for (auto iterator = m_OutputLockDescriptors.rbegin(); iterator != m_OutputLockDescriptors.rend(); ++iterator)
    {
        flock(*iterator, LOCK_UN);
        close(*iterator);
    }
    m_OutputLockDescriptors.clear();
}

void OutputTransaction::Commit()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_State != State::Staging) throw std::runtime_error("OutputTransaction: transaction is not ready to commit.");

    try
    {
        std::vector<fs::path> finalPaths;
        finalPaths.reserve(m_StagedOutputs.size());
        for (const auto &[final, _] : m_StagedOutputs)
            finalPaths.push_back(final);
        AcquireOutputLocks_(finalPaths);
        for (const auto &[final, staged] : m_StagedOutputs)
        {
            if (!fs::exists(staged)) throw std::runtime_error("OutputTransaction: staged output was not created: " + staged.string());
            Promotion promotion;
            promotion.Final = final;
            promotion.Staged = staged;
            struct stat stagedMetadata{};
            if (lstat(staged.c_str(), &stagedMetadata) != 0)
                throw std::system_error(errno, std::generic_category(), "OutputTransaction: cannot inspect staged output");
            promotion.PromotedDevice = static_cast<std::uintmax_t>(stagedMetadata.st_dev);
            promotion.PromotedInode = static_cast<std::uintmax_t>(stagedMetadata.st_ino);
            promotion.HadOriginal = fs::exists(final);
            if (promotion.HadOriginal)
            {
                const fs::path relative = final.lexically_relative(m_OutputRoot);
                promotion.Backup = m_StagingRoot / "backups" / relative;
            }
            m_Promotions.push_back(promotion);
        }
        WriteJournal_();

        for (const auto &promotion : m_Promotions)
        {
            fs::create_directories(promotion.Final.parent_path());
            if (promotion.HadOriginal)
            {
                fs::create_directories(promotion.Backup.parent_path());
                fs::rename(promotion.Final, promotion.Backup);
            }
            fs::rename(promotion.Staged, promotion.Final);
        }
        m_State = State::Promoted;
    }
    catch (...)
    {
        RollbackUnlocked_();
        throw;
    }
}

void OutputTransaction::Complete()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_State != State::Promoted && m_State != State::Staging)
        throw std::runtime_error("OutputTransaction: transaction cannot be completed in its current state.");
    std::error_code error;
    fs::remove_all(m_StagingRoot, error);
    if (error) throw std::system_error(error, "OutputTransaction: cannot remove staging directory");
    m_StagedOutputs.clear();
    m_Promotions.clear();
    m_State = State::Completed;
    ReleaseOutputLocks_();
}

void OutputTransaction::RollbackUnlocked_() noexcept
{
    std::error_code error;
    std::vector<Promotion> promotions = m_Promotions.empty() ? ReadJournal_() : m_Promotions;
    if (!AcquireRecoveryLocks_(promotions)) return;
    for (auto iterator = promotions.rbegin(); iterator != promotions.rend(); ++iterator)
    {
        struct stat finalMetadata{};
        const bool finalExists = lstat(iterator->Final.c_str(), &finalMetadata) == 0;
        const bool finalIsPromoted =
            finalExists && static_cast<std::uintmax_t>(finalMetadata.st_dev) == iterator->PromotedDevice &&
            static_cast<std::uintmax_t>(finalMetadata.st_ino) == iterator->PromotedInode;
        if (iterator->HadOriginal)
        {
            if (!fs::exists(iterator->Backup)) continue;
            if (finalExists && !finalIsPromoted) continue;
            if (finalIsPromoted) fs::remove_all(iterator->Final, error);
            error.clear();
            fs::create_directories(iterator->Final.parent_path(), error);
            error.clear();
            fs::rename(iterator->Backup, iterator->Final, error);
        }
        else if (finalIsPromoted)
        {
            fs::remove_all(iterator->Final, error);
            error.clear();
        }
    }
    if (!m_StagingRoot.empty())
    {
        fs::remove_all(m_StagingRoot, error);
        error.clear();
        const fs::path stagingParent = m_StagingRoot.parent_path();
        if (!stagingParent.empty() && fs::is_empty(stagingParent, error))
        {
            error.clear();
            fs::remove(stagingParent, error);
        }
    }
    m_StagedOutputs.clear();
    m_Promotions.clear();
    ReleaseOutputLocks_();
    if (m_State != State::Idle && m_State != State::Completed) m_State = State::RolledBack;
}

fs::path OutputTransaction::JournalPath_() const { return m_StagingRoot / "promotion-journal.txt"; }

void OutputTransaction::WriteJournal_() const
{
    if (m_Promotions.empty()) return;
    fs::create_directories(m_StagingRoot);
    const fs::path temporary = JournalPath_().string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) throw std::runtime_error("OutputTransaction: cannot create promotion journal.");
        for (const auto &promotion : m_Promotions)
            output << (promotion.HadOriginal ? 1 : 0) << ' ' << promotion.PromotedDevice << ' '
                   << promotion.PromotedInode << ' ' << std::quoted(promotion.Final.string()) << ' '
                   << std::quoted(promotion.Staged.string()) << ' ' << std::quoted(promotion.Backup.string()) << '\n';
        output.flush();
        if (!output) throw std::runtime_error("OutputTransaction: cannot write promotion journal.");
    }
    fs::rename(temporary, JournalPath_());
}

std::vector<OutputTransaction::Promotion> OutputTransaction::ReadJournal_() const noexcept
{
    std::vector<Promotion> promotions;
    if (m_StagingRoot.empty()) return promotions;
    try
    {
        std::ifstream input(JournalPath_());
        int hadOriginal = 0;
        std::uintmax_t promotedDevice = 0;
        std::uintmax_t promotedInode = 0;
        std::string final;
        std::string staged;
        std::string backup;
        while (input >> hadOriginal >> promotedDevice >> promotedInode >> std::quoted(final) >> std::quoted(staged) >>
               std::quoted(backup))
        {
            const fs::path finalPath = AbsoluteNormalized(final);
            const fs::path stagedPath = AbsoluteNormalized(staged);
            const fs::path backupPath = backup.empty() ? fs::path() : AbsoluteNormalized(backup);
            if (!IsContained_(m_OutputRoot, finalPath) || !IsContained_(m_StagingRoot, stagedPath)) continue;
            if (hadOriginal != 0 && (backupPath.empty() || !IsContained_(m_StagingRoot, backupPath))) continue;
            promotions.push_back({finalPath, stagedPath, backupPath, promotedDevice, promotedInode, hadOriginal != 0});
        }
    }
    catch (...)
    {
    }
    return promotions;
}

void OutputTransaction::Rollback() noexcept
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    RollbackUnlocked_();
}

void OutputTransaction::CleanupExternalRun() noexcept { Rollback(); }

bool OutputTransaction::IsActive() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_State == State::Staging || m_State == State::Promoted;
}

fs::path OutputTransaction::OutputRoot() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_OutputRoot;
}

fs::path OutputTransaction::StagingRoot() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_StagingRoot;
}

std::vector<std::pair<fs::path, fs::path>> OutputTransaction::StagedOutputs() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return {m_StagedOutputs.begin(), m_StagedOutputs.end()};
}

ExecutionContext::ExecutionContext()
    : m_CacheDirectory(DefaultCacheDirectory_()), m_OutputDirectory(DefaultOutputDirectory_())
{
}

fs::path ExecutionContext::DefaultCacheDirectory_()
{
    if (const char *configured = std::getenv("CASCADE_CACHE_DIR"); configured && *configured) return AbsoluteNormalized(configured);
    const char *home = std::getenv("HOME");
    return AbsoluteNormalized(home && *home ? fs::path(home) / ".cache" / "cascade" / "snapshot_cache" : fs::path(".snapshot_cache"));
}

fs::path ExecutionContext::DefaultOutputDirectory_()
{
    if (const char *configured = std::getenv("CASCADE_OUTPUT_DIR"); configured && *configured) return AbsoluteNormalized(configured);
    return AbsoluteNormalized(fs::current_path());
}

std::string ExecutionContext::MakeRunId_(const std::string &instanceName, const std::string &moduleName)
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
    std::ostringstream stream;
    stream << SafeName(instanceName.empty() ? moduleName : instanceName) << "-" << getpid() << "-" << micros << "-" << g_RunCounter.fetch_add(1);
    return stream.str();
}

void ExecutionContext::BeginRun(const std::string &instanceName, const std::string &moduleName)
{
    BeginRunWithId(instanceName, moduleName, "");
}

void ExecutionContext::BeginRunWithId(const std::string &instanceName, const std::string &moduleName,
                                      const std::string &requestedRunId)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_Active) throw std::runtime_error("ExecutionContext: a run is already active.");
    if (!requestedRunId.empty() && SafeName(requestedRunId) != requestedRunId)
        throw std::invalid_argument("ExecutionContext: requested run id contains unsupported characters.");
    m_RunId = requestedRunId.empty() ? MakeRunId_(instanceName, moduleName) : requestedRunId;
    m_Cancellation.Reset();
    m_Outputs.Begin(m_OutputDirectory, m_RunId);
    m_Active = true;
}

void ExecutionContext::CompleteRun()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Outputs.Complete();
    m_Active = false;
}

void ExecutionContext::RollbackRun() noexcept
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Outputs.Rollback();
    m_Active = false;
}

void ExecutionContext::CleanupExternalRun() noexcept
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Outputs.CleanupExternalRun();
    m_Active = false;
}

void ExecutionContext::SetCacheDirectory(const fs::path &path)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_Active) throw std::runtime_error("ExecutionContext: cannot change the cache directory during a run.");
    m_CacheDirectory = AbsoluteNormalized(path);
}

void ExecutionContext::SetOutputDirectory(const fs::path &path)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_Active) throw std::runtime_error("ExecutionContext: cannot change the output directory during a run.");
    m_OutputDirectory = AbsoluteNormalized(path);
}

fs::path ExecutionContext::CacheDirectory() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_CacheDirectory;
}

fs::path ExecutionContext::OutputDirectory() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_OutputDirectory;
}

fs::path ExecutionContext::FinalOutput(const fs::path &path) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    const fs::path final = path.is_absolute() ? AbsoluteNormalized(path) : AbsoluteNormalized(m_OutputDirectory / path);
    const fs::path relative = final.lexically_relative(m_OutputDirectory);
    if (relative.empty() || *relative.begin() == ".." || final == m_OutputDirectory)
        throw std::runtime_error("ExecutionContext: output path must be inside the configured output directory: " + final.string());
    return final;
}

fs::path ExecutionContext::StageOutput(const fs::path &path)
{
    return m_Outputs.Stage(FinalOutput(path));
}

std::string ExecutionContext::RunId() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_RunId;
}

std::string ExecutionContext::SnapshotState() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return nlohmann::json{{"schema_version", 2}, {"output_directory", m_OutputDirectory.string()}}.dump();
}

bool ExecutionContext::IsActive() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Active;
}

logger::Logger &ExecutionContext::Log() const { return logger::Logger::Get(); }
