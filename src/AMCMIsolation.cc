#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "AMCM.hh"

#include "IsolatedWorker.hh"
#include "Logger.hh"
#include "PluginPaths.hh"
#include "RuntimeOptions.hh"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <spawn.h>
#include <string>
#include <stdexcept>
#include <thread>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char **environ;

namespace
{
namespace fs = std::filesystem;

int CreateAnonymousRequestFile()
{
#if defined(__linux__) && defined(SYS_memfd_create)
    constexpr unsigned int closeOnExec = 0x0001U;
    return static_cast<int>(syscall(SYS_memfd_create, "cascade-isolated-request", closeOnExec));
#else
    errno = ENOSYS;
    return -1;
#endif
}

bool WriteAll(int descriptor, const void *data, std::size_t size)
{
    const auto *bytes = static_cast<const unsigned char *>(data);
    while (size > 0)
    {
        const ssize_t written = write(descriptor, bytes, size);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        bytes += written;
        size -= static_cast<std::size_t>(written);
    }
    return true;
}

bool ReadAll(int descriptor, void *data, std::size_t size)
{
    auto *bytes = static_cast<unsigned char *>(data);
    while (size > 0)
    {
        const ssize_t count = read(descriptor, bytes, size);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        bytes += count;
        size -= static_cast<std::size_t>(count);
    }
    return true;
}

RunResult ExternalFailure(ModuleStatus status, const std::string &message)
{
    RunResult result;
    result.Status = status;
    result.Phase = ModulePhase::Execute;
    result.Message = message;
    if (status == ModuleStatus::Failed) result.Exception = std::make_exception_ptr(std::runtime_error(message));
    return result;
}

void ValidateControlledPathParents(const fs::path &path, const std::string &label)
{
    fs::path current = path.root_path();
    for (const auto &component : path.relative_path().parent_path())
    {
        current /= component;
        struct stat metadata{};
        if (lstat(current.c_str(), &metadata) != 0 || !S_ISDIR(metadata.st_mode))
            throw std::runtime_error("Cannot inspect " + label + " parent directory: " + current.string());
        const bool writable = (metadata.st_mode & (S_IWGRP | S_IWOTH)) != 0;
        const bool controlledSticky = (metadata.st_mode & S_ISVTX) != 0 &&
                                      (metadata.st_uid == geteuid() || metadata.st_uid == 0);
        if (writable && !controlledSticky)
            throw std::runtime_error(label + " parent directory is group/world writable: " + current.string());
    }
}

std::string WorkerExecutable(const std::string &language)
{
    const char *variable = language == "python" ? "CASCADE_PYTHON_WORKER" : "CASCADE_CPP_WORKER";
    fs::path candidate;
    if (const char *configured = std::getenv(variable); configured && *configured)
    {
        candidate = configured;
        if (!candidate.is_absolute()) throw std::runtime_error(std::string(variable) + " must be an absolute path");
    }
    else
    {
        candidate = fs::path(PluginPaths::RuntimePrefix()) / "bin" /
                    (language == "python" ? "cascade-python-worker" : "cascade-worker");
    }
    std::error_code error;
    const fs::path resolved = fs::canonical(candidate, error);
    if (error || !fs::is_regular_file(resolved) || access(resolved.c_str(), X_OK) != 0)
        throw std::runtime_error("Cannot locate an executable isolated worker at " + candidate.string());
    struct stat metadata{};
    if (lstat(resolved.c_str(), &metadata) != 0 || !S_ISREG(metadata.st_mode))
        throw std::runtime_error("Cannot inspect isolated worker " + resolved.string());
    if (metadata.st_uid != geteuid() && metadata.st_uid != 0)
        throw std::runtime_error("Isolated worker is owned by another user: " + resolved.string());
    if ((metadata.st_mode & (S_IWGRP | S_IWOTH)) != 0)
        throw std::runtime_error("Isolated worker is group/world writable: " + resolved.string());
    ValidateControlledPathParents(resolved, "Isolated worker");
    return resolved.string();
}

struct SpawnEnvironment
{
    std::vector<std::string> Entries;
    std::vector<char *> Pointers;
};

SpawnEnvironment SanitizedWorkerEnvironment(const std::string &pythonRuntime)
{
    static constexpr std::array<const char *, 8> blocked = {
        "LD_PRELOAD", "LD_AUDIT", "PYTHONHOME", "PYTHONINSPECT",
        "PYTHONSTARTUP", "PYTHONBREAKPOINT", "PYTHONUSERBASE", "GCONV_PATH"};
    SpawnEnvironment result;
    for (char **entry = environ; entry && *entry; ++entry)
    {
        const std::string value(*entry);
        const auto separator = value.find('=');
        const std::string key = value.substr(0, separator);
        if (!pythonRuntime.empty() && (key == "PYTHONPATH" || key == "CASCADE_PYTHON_RUNTIME_DIR")) continue;
        if (std::find_if(blocked.begin(), blocked.end(), [&](const char *candidate) { return key == candidate; }) !=
            blocked.end())
            continue;
        result.Entries.push_back(value);
    }
    if (!pythonRuntime.empty()) result.Entries.push_back("CASCADE_PYTHON_RUNTIME_DIR=" + pythonRuntime);
    result.Pointers.reserve(result.Entries.size() + 1);
    for (auto &entry : result.Entries)
        result.Pointers.push_back(entry.data());
    result.Pointers.push_back(nullptr);
    return result;
}

std::string PythonRuntimeDirectory()
{
    fs::path candidate;
    if (const char *configured = std::getenv("CASCADE_PYTHON_RUNTIME_DIR"); configured && *configured)
    {
        candidate = configured;
        if (!candidate.is_absolute())
            throw std::runtime_error("CASCADE_PYTHON_RUNTIME_DIR must be an absolute path");
    }
    else
    {
        candidate = fs::path(PluginPaths::RuntimePrefix()) / "lib";
    }
    std::error_code error;
    const fs::path resolved = fs::canonical(candidate, error);
    if (error || !fs::is_directory(resolved))
        throw std::runtime_error("Cannot locate the isolated Python runtime at " + candidate.string());
    struct stat metadata{};
    if (lstat(resolved.c_str(), &metadata) != 0 || !S_ISDIR(metadata.st_mode))
        throw std::runtime_error("Cannot inspect isolated Python runtime " + resolved.string());
    if (metadata.st_uid != geteuid() && metadata.st_uid != 0)
        throw std::runtime_error("Isolated Python runtime is owned by another user: " + resolved.string());
    if ((metadata.st_mode & (S_IWGRP | S_IWOTH)) != 0)
        throw std::runtime_error("Isolated Python runtime is group/world writable: " + resolved.string());
    ValidateControlledPathParents(resolved / "placeholder", "Isolated Python runtime");
    return resolved.string();
}
} // namespace

RunResult AMCM::RunAModuleIsolated(const std::string &name)
{
    return RunAModuleIsolated(RegisteredModule_(name));
}

RunResult AMCM::RunAModuleIsolated(std::shared_ptr<IAnalysisModule> module)
{
    RuntimeOptionsScope runtimeScope(m_RuntimeOptions);
    module = ValidateModuleHandle_(module);
    const std::string name = module->Name();
    const std::string language = module->GetRuntimeLanguage();
    if (language != "cpp" && language != "python")
        throw std::runtime_error("Unsupported isolated module runtime: " + language);
    const auto origin = module->GetPluginOrigin();
    if (!origin)
        throw std::runtime_error("Isolated execution requires a module loaded from a verified plugin: " + module->BaseName());
    const double timeoutSeconds = m_RuntimeOptions.IsolatedTimeoutSeconds;
    const std::string executable = WorkerExecutable(language);
    const std::string pythonRuntime = language == "python" ? PythonRuntimeDirectory() : std::string();
    const nlohmann::json parameters = nlohmann::json::parse(module->DumpParamsToJSON());

    LOG_INFO("CONTROL", "Running module " << name << " in an exec worker");
    module->PrepareExternalRun();

    const RuntimeOptions runtimeOptions = m_RuntimeOptions;
    nlohmann::json request = {
        {"schema", 1},
        {"module", module->BaseName()},
        {"instance", name},
        {"runtime", language},
        {"params", parameters},
        {"cache_directory", module->GetCacheDirectory()},
        {"output_directory", module->GetOutputDirectory()},
        {"run_id", module->GetRunId()},
        {"require_signed", m_TrustPolicy == PluginTrustPolicy::RequireSigned},
        {"manifest_path", origin->ManifestPath},
        {"manifest_sha256", origin->ManifestSha256},
        {"artifact_sha256", origin->ArtifactSha256},
        {"code_sha256", origin->CodeSha256.empty() ? origin->ArtifactSha256 : origin->CodeSha256},
        {"runtime_options",
         {{"input_hash", runtimeOptions.InputHash},
          {"output_hash", runtimeOptions.OutputHash},
          {"dag_workers", runtimeOptions.DagWorkers},
          {"progress_interval_ms", runtimeOptions.ProgressIntervalMilliseconds},
          {"artifact_hash_cache_entries", runtimeOptions.ArtifactHashCacheEntries}}},
    };
    const std::string payload = request.dump();

    int input = CreateAnonymousRequestFile();
    int channel[2] = {-1, -1};
    if (input < 0 || !WriteAll(input, payload.data(), payload.size()) || lseek(input, 0, SEEK_SET) < 0 ||
        pipe2(channel, O_CLOEXEC) != 0)
    {
        const std::string message = "Cannot create isolated execution channel: " + std::string(std::strerror(errno));
        if (input >= 0) close(input);
        RunResult result = module->AdoptExternalRunResult(ExternalFailure(ModuleStatus::Failed, message));
        RecordRun_(module, result);
        return result;
    }

    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    int resultDescriptor = 10;
    while (resultDescriptor == input || resultDescriptor == channel[0] || resultDescriptor == channel[1])
        ++resultDescriptor;
    posix_spawn_file_actions_init(&actions);
    posix_spawnattr_init(&attributes);
    posix_spawn_file_actions_adddup2(&actions, input, STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, channel[1], resultDescriptor);
    posix_spawn_file_actions_addclose(&actions, input);
    posix_spawn_file_actions_addclose(&actions, channel[0]);
    posix_spawn_file_actions_addclose(&actions, channel[1]);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);

    const std::string resultDescriptorText = std::to_string(resultDescriptor);
    char *workerArguments[] = {const_cast<char *>(executable.c_str()),
                               const_cast<char *>(resultDescriptorText.c_str()), nullptr};
    auto workerEnvironment = SanitizedWorkerEnvironment(pythonRuntime);
    pid_t child = -1;
    const int spawnError = posix_spawn(&child, executable.c_str(), &actions, &attributes, workerArguments,
                                       workerEnvironment.Pointers.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    close(input);
    close(channel[1]);
    if (spawnError != 0)
    {
        const std::string message = "Cannot start isolated worker '" + executable + "': " + std::string(std::strerror(spawnError));
        close(channel[0]);
        RunResult result = module->AdoptExternalRunResult(ExternalFailure(ModuleStatus::Failed, message));
        RecordRun_(module, result);
        return result;
    }

    int childStatus = 0;
    bool waitFailed = false;
    bool cancellationSent = false;
    int cancellationPolls = 0;
    bool timedOut = false;
    const auto startedAt = std::chrono::steady_clock::now();
    while (true)
    {
        const pid_t waited = waitpid(child, &childStatus, WNOHANG);
        if (waited == child) break;
        if (waited < 0 && errno != EINTR)
        {
            waitFailed = true;
            break;
        }
        if (timeoutSeconds > 0.0 &&
            std::chrono::duration<double>(std::chrono::steady_clock::now() - startedAt).count() >= timeoutSeconds)
        {
            kill(-child, SIGKILL);
            timedOut = true;
        }
        else if (module->IsCancellationRequested())
        {
            if (!cancellationSent)
            {
                kill(-child, SIGTERM);
                cancellationSent = true;
            }
            else if (++cancellationPolls >= 50)
            {
                kill(-child, SIGKILL);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    RunResult externalResult;
    if (timedOut)
        externalResult = ExternalFailure(ModuleStatus::Failed, "Isolated module exceeded its configured timeout");
    else if (cancellationSent)
        externalResult = ExternalFailure(ModuleStatus::Interrupted, "Isolated module was cancelled");
    else if (waitFailed)
        externalResult = ExternalFailure(ModuleStatus::Failed, "Failed while waiting for isolated worker");
    else if (WIFSIGNALED(childStatus))
        externalResult = ExternalFailure(ModuleStatus::Failed,
                                         "Isolated module terminated by signal " + std::to_string(WTERMSIG(childStatus)));
    else
    {
        const int channelFlags = fcntl(channel[0], F_GETFL, 0);
        if (channelFlags < 0 || fcntl(channel[0], F_SETFL, channelFlags | O_NONBLOCK) != 0)
        {
            externalResult = ExternalFailure(ModuleStatus::Failed, "Cannot make isolated result channel non-blocking");
            close(channel[0]);
            RunResult result = module->AdoptExternalRunResult(std::move(externalResult));
            RecordRun_(module, result);
            return result;
        }
        IsolatedRunHeader header;
        if (!ReadAll(channel[0], &header, sizeof(header)) || header.Magic != kCascadeWorkerResultMagic ||
            header.MessageSize > kCascadeWorkerMaxMessageSize ||
            header.CacheDecisionSize > kCascadeWorkerMaxCacheDetailSize ||
            header.CacheReasonSize > kCascadeWorkerMaxCacheDetailSize)
        {
            externalResult = ExternalFailure(ModuleStatus::Failed, "Isolated module exited without a valid result");
        }
        else
        {
            std::string message(header.MessageSize, '\0');
            std::string cacheDecision(header.CacheDecisionSize, '\0');
            std::string cacheReason(header.CacheReasonSize, '\0');
            if (!ReadAll(channel[0], message.data(), message.size()) ||
                !ReadAll(channel[0], cacheDecision.data(), cacheDecision.size()) ||
                !ReadAll(channel[0], cacheReason.data(), cacheReason.size()))
                externalResult = ExternalFailure(ModuleStatus::Failed, "Isolated module result was truncated");
            else if (header.Status < static_cast<std::int32_t>(ModuleStatus::Pending) ||
                     header.Status > static_cast<std::int32_t>(ModuleStatus::Failed) ||
                     header.Phase < static_cast<std::int32_t>(ModulePhase::None) ||
                     header.Phase > static_cast<std::int32_t>(ModulePhase::Commit))
                externalResult = ExternalFailure(ModuleStatus::Failed, "Isolated module returned invalid status values");
            else
            {
                externalResult.Status = static_cast<ModuleStatus>(header.Status);
                externalResult.Phase = static_cast<ModulePhase>(header.Phase);
                externalResult.Message = std::move(message);
                externalResult.CacheDecision = std::move(cacheDecision);
                externalResult.CacheReason = std::move(cacheReason);
            }
        }
    }
    close(channel[0]);

    RunResult result = module->AdoptExternalRunResult(std::move(externalResult));
    RecordRun_(module, result);
    LOG_INFO("CONTROL", "Isolated module " << name << " finished with status " << ToString(result.Status));
    return result;
}
