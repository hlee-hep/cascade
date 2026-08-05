#include "AMCM.hh"
#include "Provenance.hh"
#include "AnalysisModuleRegistry.hh"
#include "InterruptManager.hh"
#include "IsolatedWorker.hh"
#include "ExecutionResources.hh"
#include "Logger.hh"
#include "PluginABI.hh"
#include "PluginPaths.hh"
#include "PluginVerifier.hh"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <filesystem>
#include <mutex>
#include <nlohmann/json.hpp>
#include <spawn.h>
#include <set>
#include <thread>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <yaml-cpp/yaml.h>
#include <vector>

extern char **environ;

namespace
{
namespace fs = std::filesystem;

std::vector<fs::path> CppPluginRoots()
{
    std::vector<fs::path> roots;
    for (const auto &root : PluginPaths::Roots("cpp")) roots.emplace_back(root);
    return roots;
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

double IsolatedTimeoutSeconds()
{
    const char *configured = std::getenv("CASCADE_ISOLATED_TIMEOUT_SECONDS");
    if (!configured || !*configured) return 0.0;
    char *end = nullptr;
    errno = 0;
    const double value = std::strtod(configured, &end);
    if (errno != 0 || end == configured || *end != '\0' || !std::isfinite(value) || value < 0.0)
        throw std::runtime_error("CASCADE_ISOLATED_TIMEOUT_SECONDS must be a non-negative number");
    return value;
}

void LoadVerifiedCppPackages(const std::vector<VerifiedPluginPackage> &packages)
{
    static std::mutex pluginLoadMutex;
    static std::map<std::string, std::string> loadedPlugins;
    static std::vector<int> loadedDescriptors;
    for (const auto &package : packages)
    {
        LOG_DEBUG("PLUGIN", ToString(package.Trust) << " package " << package.Package);
        for (const auto &plugin : package.Artifacts)
        {
            std::lock_guard<std::mutex> loadLock(pluginLoadMutex);
            const fs::path pluginFile(plugin.Path);
            const std::string canonicalPlugin = plugin.Path;
            const auto existingModule = AnalysisModuleRegistry::Get().GetPluginOrigin(plugin.Name);
            if (existingModule && existingModule->Package == plugin.Origin.Package &&
                existingModule->ArtifactSha256 != plugin.Sha256)
                throw std::runtime_error("Installed C++ plugin changed and requires a new process: " +
                                         plugin.Name);
            const auto loaded = loadedPlugins.find(canonicalPlugin);
            if (loaded != loadedPlugins.end())
            {
                if (loaded->second != plugin.Sha256)
                    throw std::runtime_error("Installed C++ plugin changed and requires a new process: " +
                                             canonicalPlugin);
                for (const auto &name : AnalysisModuleRegistry::Get().ListModules())
                {
                    const auto existing = AnalysisModuleRegistry::Get().GetPluginOrigin(name);
                    if (existing && existing->ArtifactSha256 == plugin.Origin.ArtifactSha256 &&
                        existing->Package == plugin.Origin.Package && plugin.Origin.Trust == PluginTrustStatus::Signed)
                        AnalysisModuleRegistry::Get().SetPluginOrigin(name, plugin.Origin);
                }
                continue;
            }
            const auto modulesBeforeLoad = AnalysisModuleRegistry::Get().ListModules();
            const std::set<std::string> moduleSetBeforeLoad(modulesBeforeLoad.begin(), modulesBeforeLoad.end());
            auto rollbackRegistrations = [&]()
            {
                for (const auto &module : AnalysisModuleRegistry::Get().ListModules())
                    if (!moduleSetBeforeLoad.count(module)) AnalysisModuleRegistry::Get().Unregister(module);
            };
            fs::path loadPath = pluginFile;
            int retainedDescriptor = -1;
#if defined(__linux__)
            if (plugin.Descriptor() >= 0)
            {
                retainedDescriptor = dup(plugin.Descriptor());
                const fs::path descriptorPath = fs::path("/proc/self/fd") / std::to_string(retainedDescriptor);
                if (retainedDescriptor >= 0 && fs::exists(descriptorPath)) loadPath = descriptorPath;
            }
#elif defined(__APPLE__)
            if (plugin.Descriptor() >= 0)
            {
                retainedDescriptor = dup(plugin.Descriptor());
                const fs::path descriptorPath = fs::path("/dev/fd") / std::to_string(retainedDescriptor);
                if (retainedDescriptor >= 0 && fs::exists(descriptorPath)) loadPath = descriptorPath;
            }
#endif
            void *handle = dlopen(loadPath.c_str(), RTLD_NOW);
            if (!handle) LOG_WARN("PLUGIN", "dlopen failed for '" << pluginFile.string() << "': " << dlerror());
            if (!handle)
            {
                if (retainedDescriptor >= 0) close(retainedDescriptor);
                continue;
            }
            if (AnalysisModuleRegistry::Get().ListModules() != modulesBeforeLoad)
            {
                LOG_ERROR("PLUGIN", "Plugin performed static module registration before ABI validation: " << pluginFile.string());
                rollbackRegistrations();
                dlclose(handle);
                if (retainedDescriptor >= 0) close(retainedDescriptor);
                continue;
            }
            dlerror();
            using AbiFn = int (*)();
            using AbiTagFn = const char *(*)();
            using RegisterFn = void (*)();
            auto abiFn = reinterpret_cast<AbiFn>(dlsym(handle, "CascadePluginAbiVersion"));
            const char *abiErr = dlerror();
            if (abiErr) abiFn = nullptr;
            dlerror();
            auto abiTagFn = reinterpret_cast<AbiTagFn>(dlsym(handle, "CascadePluginAbiTag"));
            const char *abiTagErr = dlerror();
            if (abiTagErr) abiTagFn = nullptr;
            dlerror();
            auto regFn = reinterpret_cast<RegisterFn>(dlsym(handle, "CascadeRegisterPlugin"));
            const char *regErr = dlerror();
            if (regErr) regFn = nullptr;

            if (!abiFn || !abiTagFn || !regFn)
            {
                LOG_ERROR("PLUGIN", "Plugin is missing required ABI or registration entry points: " << pluginFile.string());
                rollbackRegistrations();
                dlclose(handle);
                if (retainedDescriptor >= 0) close(retainedDescriptor);
                continue;
            }

            const int abi = abiFn();
            if (abi != CASCADE_PLUGIN_ABI_VERSION)
            {
                LOG_ERROR("PLUGIN", "Plugin ABI mismatch for '" << pluginFile.string() << "': " << abi << " != " << CASCADE_PLUGIN_ABI_VERSION);
                rollbackRegistrations();
                dlclose(handle);
                if (retainedDescriptor >= 0) close(retainedDescriptor);
                continue;
            }
            const char *rawTag = abiTagFn();
            if (!rawTag || std::string(rawTag) != CASCADE_ABI_TAG)
            {
                LOG_ERROR("PLUGIN", "Plugin ABI tag mismatch for '" << pluginFile.string() << "'");
                rollbackRegistrations();
                dlclose(handle);
                if (retainedDescriptor >= 0) close(retainedDescriptor);
                continue;
            }

            try
            {
                regFn();
                const auto modulesAfterLoad = AnalysisModuleRegistry::Get().ListModules();
                std::vector<std::string> registered;
                for (const auto &name : modulesAfterLoad)
                    if (!moduleSetBeforeLoad.count(name)) registered.push_back(name);
                if (registered.size() != 1 || registered.front() != plugin.Name)
                    throw std::runtime_error("plugin registration identity does not match verified manifest: expected " +
                                             plugin.Name);
                AnalysisModuleRegistry::Get().SetPluginOrigin(plugin.Name, plugin.Origin);
                loadedPlugins.emplace(canonicalPlugin, plugin.Sha256);
                if (retainedDescriptor >= 0) loadedDescriptors.push_back(retainedDescriptor);
                LOG_INFO("PLUGIN", "Loaded " << ToString(plugin.Origin.Trust) << " plugin " << pluginFile.string());
            }
            catch (const std::exception &error)
            {
                LOG_ERROR("PLUGIN", "Plugin registration failed for '" << pluginFile.string() << "': " << error.what());
                rollbackRegistrations();
                dlclose(handle);
                if (retainedDescriptor >= 0) close(retainedDescriptor);
            }
            catch (...)
            {
                LOG_ERROR("PLUGIN", "Plugin registration failed for '" << pluginFile.string() << "' with an unknown exception");
                rollbackRegistrations();
                dlclose(handle);
                if (retainedDescriptor >= 0) close(retainedDescriptor);
            }
        }
    }
}
} // namespace

AMCM::AMCM() : AMCM(PluginTrustPolicy::Verified) {}

AMCM::AMCM(PluginTrustPolicy trustPolicy) : AMCM(trustPolicy, true) {}

AMCM::AMCM(PluginTrustPolicy trustPolicy, bool discoverPlugins)
    : m_TrustPolicy(trustPolicy), m_IndexPlugins(discoverPlugins)
{
    InterruptManager::Init();
    m_Dag = std::make_unique<DAGManager>();
    if (m_IndexPlugins) RefreshPluginIndex_();
}

std::shared_ptr<IAnalysisModule> AMCM::RegisterModule(const std::string &base, const std::string &instanceName)
{
    if (instanceName.empty()) throw std::invalid_argument("Module instance name cannot be empty");
    std::lock_guard<std::recursive_mutex> registrationLock(m_RegistrationMutex);
    {
        std::lock_guard<std::mutex> lock(m_ControlMutex);
        if (m_Modules.count(instanceName)) throw std::runtime_error("Module instance already registered: " + instanceName);
    }
    EnsureCppPluginLoaded_(base);
    const auto origin = AnalysisModuleRegistry::Get().GetPluginOrigin(base);
    if (m_TrustPolicy == PluginTrustPolicy::RequireSigned && origin && origin->Trust != PluginTrustStatus::Signed)
        throw std::runtime_error("Module requires a signed plugin under the active trust policy: " + base);
    auto mod = AnalysisModuleRegistry::Get().Create(base);
    if (origin)
    {
        mod->SetBaseName(base);
        mod->SetCodeHash("artifact-sha256:" + origin->ArtifactSha256);
    }
    mod->SetPluginOrigin(origin);
    mod->SetName(instanceName);
    auto ptr = std::shared_ptr<IAnalysisModule>(std::move(mod));
    {
        std::lock_guard<std::mutex> lock(m_ControlMutex);
        m_Modules[instanceName] = ptr;
    }
    LOG_INFO("CONTROL", "Module " << base << " is registered as " << instanceName);
    return ptr;
}

std::vector<std::string> AMCM::ListAvailableModules() const
{
    std::lock_guard<std::recursive_mutex> registrationLock(m_RegistrationMutex);
    auto modules = AnalysisModuleRegistry::Get().ListModules();
    if (m_TrustPolicy == PluginTrustPolicy::RequireSigned)
        modules.erase(std::remove_if(modules.begin(), modules.end(),
                                     [](const std::string &name)
                                     {
                                         const auto origin = AnalysisModuleRegistry::Get().GetPluginOrigin(name);
                                         return origin && origin->Trust != PluginTrustStatus::Signed;
                                     }),
                      modules.end());
    std::set<std::string> available(modules.begin(), modules.end());
    std::set<std::string> indexed;
    for (const auto &candidate : m_CppPluginIndex)
    {
        if (m_TrustPolicy == PluginTrustPolicy::RequireSigned && !candidate.HasSignature) continue;
        if (!indexed.insert(candidate.Identity).second)
            throw std::runtime_error("Duplicate C++ plugin module name in manifest index: " + candidate.Identity);
        available.insert(candidate.Identity);
    }
    modules.assign(available.begin(), available.end());
    return modules;
}

std::vector<ModuleMetadata> AMCM::ListAvailableModuleMetadata() const
{
    std::lock_guard<std::recursive_mutex> registrationLock(m_RegistrationMutex);
    auto metadata = AnalysisModuleRegistry::Get().ListModuleMetadata();
    if (m_TrustPolicy == PluginTrustPolicy::RequireSigned)
        metadata.erase(std::remove_if(metadata.begin(), metadata.end(),
                                      [](const ModuleMetadata &item)
                                      {
                                          const auto origin = AnalysisModuleRegistry::Get().GetPluginOrigin(item.Name);
                                          return origin && origin->Trust != PluginTrustStatus::Signed;
                                      }),
                       metadata.end());
    std::map<std::string, ModuleMetadata> available;
    for (auto &item : metadata) available[item.Name] = std::move(item);
    std::set<std::string> indexed;
    for (const auto &candidate : m_CppPluginIndex)
    {
        if (m_TrustPolicy == PluginTrustPolicy::RequireSigned && !candidate.HasSignature) continue;
        if (!indexed.insert(candidate.Identity).second)
            throw std::runtime_error("Duplicate C++ plugin module name in manifest index: " + candidate.Identity);
        if (!available.count(candidate.Identity)) available[candidate.Identity] = candidate.Metadata;
    }
    metadata.clear();
    for (auto &[_, item] : available) metadata.push_back(std::move(item));
    return metadata;
}

std::optional<PluginOrigin> AMCM::GetPluginOrigin(const std::string &name) const
{
    return AnalysisModuleRegistry::Get().GetPluginOrigin(name);
}

std::shared_ptr<IAnalysisModule> AMCM::RegisterModule(const std::string &base)
{
    int count = 0;
    {
        std::lock_guard<std::mutex> lock(m_ControlMutex);
        count = ++m_ModuleNameCounter[base];
    }
    std::string autoName = base + "_" + std::to_string(count);
    return RegisterModule(base, autoName);
}

std::shared_ptr<IAnalysisModule> AMCM::RegisterModuleHandle(std::shared_ptr<IAnalysisModule> module)
{
    if (!module) throw std::invalid_argument("Cannot register a null module");
    if (module->Name().empty()) throw std::invalid_argument("Module instance name cannot be empty");
    std::lock_guard<std::recursive_mutex> registrationLock(m_RegistrationMutex);
    std::lock_guard<std::mutex> controlLock(m_ControlMutex);
    if (m_Modules.count(module->Name()))
        throw std::runtime_error("Module instance already registered: " + module->Name());
    m_Modules[module->Name()] = module;
    LOG_INFO("CONTROL", "Module " << module->BaseName() << " is registered as " << module->Name());
    return module;
}

std::vector<std::string> AMCM::ListRegisteredModules() const
{
    std::vector<std::string> names = {};
    std::lock_guard<std::mutex> lock(m_ControlMutex);
    for (auto &[_, mod] : m_Modules)
        names.push_back(mod->Name());

    return names;
}

std::shared_ptr<IAnalysisModule> AMCM::GetModule(const std::string &name)
{
    std::lock_guard<std::mutex> lock(m_ControlMutex);
    auto it = m_Modules.find(name);
    if (it == m_Modules.end()) throw std::runtime_error("Module not registered: " + name);
    return it->second;
}

std::string AMCM::GetStatus(const std::string &name) const
{
    std::lock_guard<std::mutex> lock(m_ControlMutex);
    if (m_Modules.count(name) == 0) throw std::runtime_error("Module not found");
    return m_Modules.at(name)->GetStatus();
}

std::map<std::string, std::map<std::string, double>> AMCM::GetAllProgress() const
{
    std::map<std::string, std::map<std::string, double>> result;
    std::lock_guard<std::mutex> lock(m_ControlMutex);
    for (const auto &[modName, mod] : m_Modules)
    {
        result[modName] = mod->GetProgressSnapshot();
    }
    return result;
}

RunResult AMCM::RunAModule(const std::string &name)
{
    auto mod = RegisteredModule_(name);
    LOG_INFO("CONTROL", "Running module " << name);
    std::unique_lock<std::recursive_mutex> rootLock(CascadeRootExecutionMutex(), std::defer_lock);
    if (mod->RequiresRootSerialization()) rootLock.lock();
    RunResult result = mod->Run();
    RecordRun_(mod, result);
    LOG_INFO("CONTROL", "Module " << name << " finished execution with status " << ToString(result.Status));
    return result;
}

RunResult AMCM::RunAModule(std::shared_ptr<IAnalysisModule> mod)
{
    mod = ValidateModuleHandle_(mod);
    LOG_INFO("CONTROL", "Running module " << mod->Name());
    std::unique_lock<std::recursive_mutex> rootLock(CascadeRootExecutionMutex(), std::defer_lock);
    if (mod->RequiresRootSerialization()) rootLock.lock();
    RunResult result = mod->Run();
    RecordRun_(mod, result);
    LOG_INFO("CONTROL", "Module " << mod->Name() << " finished execution with status " << ToString(result.Status));
    return result;
}

void AMCM::RecordRun_(const std::shared_ptr<IAnalysisModule> &module, const RunResult &result)
{
    std::lock_guard<std::mutex> lock(m_ControlMutex);
    m_ExecutedModules.push_back(
        {module->GetRunId(), module->GetLastProvenancePath(), module->Name(), module->BaseName(), result});
}

RunResult AMCM::RunAModuleIsolated(const std::string &name)
{
    return RunAModuleIsolated(RegisteredModule_(name));
}

RunResult AMCM::RunAModuleIsolated(std::shared_ptr<IAnalysisModule> module)
{
    module = ValidateModuleHandle_(module);
    const std::string name = module->Name();
    const std::string language = module->GetRuntimeLanguage();
    if (language != "cpp" && language != "python")
        throw std::runtime_error("Unsupported isolated module runtime: " + language);
    const auto origin = module->GetPluginOrigin();
    if (!origin)
        throw std::runtime_error("Isolated execution requires a module loaded from a verified plugin: " + module->BaseName());
    const double timeoutSeconds = IsolatedTimeoutSeconds();
    const std::string executable = WorkerExecutable(language);
    const std::string pythonRuntime = language == "python" ? PythonRuntimeDirectory() : std::string();
    const nlohmann::json parameters = nlohmann::json::parse(module->DumpParamsToJSON());

    LOG_INFO("CONTROL", "Running module " << name << " in an exec worker");
    module->PrepareExternalRun();

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
    };
    const std::string payload = request.dump();

    int input = memfd_create("cascade-isolated-request", MFD_CLOEXEC);
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
    {
        externalResult = ExternalFailure(ModuleStatus::Failed, "Isolated module exceeded its configured timeout");
    }
    else if (cancellationSent)
    {
        externalResult = ExternalFailure(ModuleStatus::Interrupted, "Isolated module was cancelled");
    }
    else if (waitFailed)
    {
        externalResult = ExternalFailure(ModuleStatus::Failed, "Failed while waiting for isolated worker");
    }
    else if (WIFSIGNALED(childStatus))
    {
        externalResult = ExternalFailure(ModuleStatus::Failed,
                                         "Isolated module terminated by signal " + std::to_string(WTERMSIG(childStatus)));
    }
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

std::vector<RunResult> AMCM::SequentialRun(bool failFast)
{
    LOG_INFO("CONTROL", "Sequential Run is starting.");
    auto results = RunModules(ListRegisteredModules(), failFast);
    LOG_INFO("CONTROL", "Sequential Run is ended.");
    return results;
}

std::vector<RunResult> AMCM::RunModules(const std::vector<std::string> &group, bool failFast)
{
    LOG_INFO("CONTROL", "Running " << group.size() << " modules from provided list");
    std::vector<RunResult> results;
    results.reserve(group.size());
    for (const auto &name : group)
    {
        results.push_back(RunAModule(name));
        if (failFast && !results.back().AllowsDependents()) break;
    }
    LOG_INFO("CONTROL", "Finished running provided module list");
    return results;
}

std::vector<RunResult> AMCM::RunModules(std::vector<std::shared_ptr<IAnalysisModule>> group, bool failFast)
{
    LOG_INFO("CONTROL", "Running " << group.size() << " provided module handles");
    std::vector<RunResult> results;
    results.reserve(group.size());
    for (const auto &mod : group)
    {
        results.push_back(RunAModule(mod));
        if (failFast && !results.back().AllowsDependents()) break;
    }
    LOG_INFO("CONTROL", "Finished running provided module handles");
    return results;
}

void AMCM::AddModuleToDAG(const std::string &name, const std::vector<std::string> &dependencies, bool isolated)
{
    const auto module = RegisteredModule_(name);
    DAGExecutionLane lane = DAGExecutionLane::Parallel;
    if (isolated)
        lane = DAGExecutionLane::Isolated;
    else if (module->RequiresRootSerialization())
        lane = DAGExecutionLane::Root;
    m_Dag->AddNode(
        name, dependencies,
        [this, name, isolated]()
        {
            const RunResult result = isolated ? RunAModuleIsolated(name) : RunAModule(name);
            if (!result.AllowsDependents())
                throw std::runtime_error("Module " + name + " finished with status " + ToString(result.Status) +
                                         (result.Message.empty() ? std::string() : ": " + result.Message));
        },
        lane);
}

void AMCM::LinkDAGModuleParameter(const std::string &fromNode, const std::string &fromKey, const std::string &toNode,
                                  const std::string &toKey)
{
    auto source = RegisteredModule_(fromNode);
    auto target = RegisteredModule_(toNode);
    if (!source->HasParam(fromKey)) throw std::runtime_error("DAG source parameter is not registered: " + fromNode + "." + fromKey);
    if (!target->HasParam(toKey)) throw std::runtime_error("DAG target parameter is not registered: " + toNode + "." + toKey);
    m_Dag->AddDataLink(
        fromNode, toNode, fromKey + " -> " + toKey,
        [source = std::move(source), target = std::move(target), fromKey, toKey]()
        { target->SetParamValue(toKey, source->GetParamValue(fromKey)); });
}

DAGRunResult AMCM::RunDAG(bool failFast)
{
    std::lock_guard<std::recursive_mutex> registrationLock(m_RegistrationMutex);
    {
        std::lock_guard<std::mutex> controlLock(m_ControlMutex);
        m_ExecutedModules.clear();
    }
    LOG_INFO("CONTROL", "Executing DAG workflow");
    auto result = m_Dag->Execute(failFast);
    LOG_INFO("CONTROL", "DAG workflow execution completed");
    return result;
}

std::shared_ptr<IAnalysisModule> AMCM::RegisteredModule_(const std::string &name) const
{
    std::lock_guard<std::mutex> lock(m_ControlMutex);
    const auto iterator = m_Modules.find(name);
    if (iterator == m_Modules.end()) throw std::runtime_error("Module not registered: " + name);
    return iterator->second;
}

std::shared_ptr<IAnalysisModule> AMCM::ValidateModuleHandle_(const std::shared_ptr<IAnalysisModule> &module) const
{
    if (!module) throw std::invalid_argument("Cannot run a null module");
    const auto registered = RegisteredModule_(module->Name());
    if (registered != module) throw std::runtime_error("Module handle is not owned by this controller: " + module->Name());
    return registered;
}

std::string AMCM::SaveProvenance(const std::string &path, bool failFast) const
{
    std::lock_guard<std::mutex> lock(m_ControlMutex);
    WorkflowRunManifest workflow;
    workflow.RunId = ProvenanceRecorder::MakeWorkflowRunId();
    workflow.FailFast = failFast;
    workflow.Succeeded = true;
    std::set<std::string> languages;

    std::map<std::string, ModuleRunManifest> manifestsByInstance;
    for (const auto &entry : m_ExecutedModules)
    {
        std::optional<ModuleRunManifest> manifest = ProvenanceRecorder::FindModuleRun(entry.RunId);
        if (!manifest && !entry.ManifestPath.empty() && std::filesystem::is_regular_file(entry.ManifestPath))
            manifest = ProvenanceRecorder::LoadModuleRun(entry.ManifestPath);
        if (manifest)
        {
            if (!manifest->Runtime.Language.empty()) languages.insert(manifest->Runtime.Language);
            manifestsByInstance[entry.InstanceName] = *manifest;
            workflow.ModuleManifestPaths.push_back(manifest->ManifestPath);
            if (workflow.StartedAt.empty() || manifest->StartedAt < workflow.StartedAt) workflow.StartedAt = manifest->StartedAt;
            if (workflow.FinishedAt.empty() || manifest->FinishedAt > workflow.FinishedAt) workflow.FinishedAt = manifest->FinishedAt;
        }
        workflow.Succeeded = workflow.Succeeded && entry.Result.AllowsDependents();
    }

    const auto dependencies = m_Dag->GetDependencies();
    const auto dagResults = m_Dag->GetNodeResults();
    if (!dagResults.empty())
    {
        for (const auto &result : dagResults)
        {
            WorkflowNodeProvenance node;
            node.Name = result.Name;
            node.Status = ToString(result.Status);
            node.Message = result.Message;
            const auto dependency = dependencies.find(result.Name);
            if (dependency != dependencies.end()) node.Dependencies = dependency->second;
            const auto module = manifestsByInstance.find(result.Name);
            if (module != manifestsByInstance.end())
            {
                node.ModuleRunId = module->second.RunId;
                node.ModuleManifestPath = module->second.ManifestPath;
            }
            workflow.Nodes.push_back(std::move(node));
        }
        for (const auto &link : m_Dag->GetDataLinks())
            workflow.DataLinks.push_back({link.FromNode, link.ToNode, link.Label});
        workflow.Succeeded = workflow.Succeeded &&
                             std::all_of(dagResults.begin(), dagResults.end(),
                                         [](const DAGNodeResult &node) { return node.Status == DAGNodeStatus::Succeeded; });
    }
    else
    {
        for (const auto &entry : m_ExecutedModules)
        {
            WorkflowNodeProvenance node;
            node.Name = entry.InstanceName;
            node.Status = ToString(entry.Result.Status);
            node.Message = entry.Result.Message;
            node.ModuleRunId = entry.RunId;
            node.ModuleManifestPath = entry.ManifestPath;
            workflow.Nodes.push_back(std::move(node));
        }
    }

    if (workflow.StartedAt.empty()) workflow.StartedAt = ProvenanceRecorder::NowUTC();
    if (workflow.FinishedAt.empty()) workflow.FinishedAt = ProvenanceRecorder::NowUTC();
    workflow.Runtime = ProvenanceRecorder::Runtime(
        languages.size() > 1 ? "mixed" : (languages.empty() ? "cpp" : *languages.begin()));
    const std::string saved = ProvenanceRecorder::WriteWorkflowRun(workflow, path);
    LOG_INFO("CONTROL", "Workflow provenance '" << saved << "' is saved.");
    return saved;
}

void AMCM::SaveRunLog() const
{
    SaveProvenance();
}

void AMCM::LoadPlugins(const std::string &path)
{
    std::lock_guard<std::recursive_mutex> registrationLock(m_RegistrationMutex);
    const fs::path pluginRoot(path);
    if (!fs::is_directory(pluginRoot))
    {
        LOG_WARN("CONTROL", "Plugin directory not found: " << path);
        return;
    }
    auto discovery = PluginVerifier::Discover({pluginRoot.string()}, m_TrustPolicy, "cpp");
    for (const auto &error : discovery.Errors) LOG_WARN("PLUGIN", error);
    LoadVerifiedCppPackages(discovery.Packages);
}

void AMCM::LoadPluginPackage(const std::string &manifestPath, const std::string &moduleName)
{
    std::lock_guard<std::recursive_mutex> registrationLock(m_RegistrationMutex);
    const fs::path manifest = fs::canonical(manifestPath);
    if (manifest.filename() != "plugin_manifest.json")
        throw std::runtime_error("Targeted plugin load requires a plugin_manifest.json path");
    const fs::path packageDirectory = manifest.parent_path();
    const fs::path pluginRoot = packageDirectory.parent_path();
    auto package = PluginVerifier::VerifyPackage(packageDirectory.string(),
                                                 PluginPaths::TrustStoreForRoot(pluginRoot.string()),
                                                 m_TrustPolicy, "cpp", "", moduleName);
    if (package.ManifestPath != manifest.string())
        throw std::runtime_error("Targeted plugin manifest changed during verification");
    LoadVerifiedCppPackages({package});
}

std::vector<std::string> AMCM::RefreshPlugins()
{
    if (m_Dag->IsExecuting()) throw std::runtime_error("Cannot refresh plugins while the DAG is executing");
    std::lock_guard<std::recursive_mutex> registrationLock(m_RegistrationMutex);
    auto before = ListAvailableModules();
    RefreshPluginIndex_();
    auto after = ListAvailableModules();
    std::sort(before.begin(), before.end());
    std::sort(after.begin(), after.end());
    std::vector<std::string> added;
    std::set_difference(after.begin(), after.end(), before.begin(), before.end(), std::back_inserter(added));
    return added;
}

void AMCM::RefreshPluginIndex_()
{
    if (!m_IndexPlugins)
    {
        m_CppPluginIndex.clear();
        return;
    }
    std::vector<std::string> roots;
    for (const auto &root : CppPluginRoots()) roots.push_back(root.string());
    auto index = PluginVerifier::IndexManifests(roots, "cpp");
    for (const auto &error : index.Errors) LOG_WARN("PLUGIN", error);
    for (const auto &candidate : index.Entries)
    {
        const auto origin = AnalysisModuleRegistry::Get().GetPluginOrigin(candidate.Identity);
        if (origin && origin->Package == candidate.Package &&
            origin->ArtifactSha256 != candidate.DeclaredSha256)
            throw std::runtime_error("Installed C++ plugin changed and requires a new process: " +
                                     candidate.Identity);
    }
    m_CppPluginIndex = std::move(index.Entries);
}

void AMCM::EnsureCppPluginLoaded_(const std::string &base)
{
    const auto loaded = AnalysisModuleRegistry::Get().ListModules();
    if (std::find(loaded.begin(), loaded.end(), base) != loaded.end()) return;
    if (!m_IndexPlugins) throw std::runtime_error("Module not found: " + base);
    std::vector<const PluginManifestEntry *> matches;
    for (const auto &candidate : m_CppPluginIndex)
        if (candidate.Identity == base) matches.push_back(&candidate);
    if (matches.empty()) throw std::runtime_error("Module not found: " + base);
    if (matches.size() != 1)
        throw std::runtime_error("Duplicate C++ plugin module name in manifest index: " + base);
    if (m_TrustPolicy == PluginTrustPolicy::RequireSigned && !matches.front()->HasSignature)
        throw std::runtime_error("Module requires a signed plugin under the active trust policy: " + base);
    LoadPluginPackage(matches.front()->ManifestPath, base);
    const auto after = AnalysisModuleRegistry::Get().ListModules();
    if (std::find(after.begin(), after.end(), base) == after.end())
        throw std::runtime_error("Plugin failed to register module: " + base);
}
