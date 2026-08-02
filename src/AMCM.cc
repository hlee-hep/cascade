#include "AMCM.hh"
#include "Provenance.hh"
#include "AnalysisModuleRegistry.hh"
#include "InterruptManager.hh"
#include "Logger.hh"
#include "PluginABI.hh"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <sstream>
#include <set>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>
#include <yaml-cpp/yaml.h>

namespace
{
namespace fs = std::filesystem;

struct TrustedKey
{
    fs::path Path;
    EVP_PKEY *Value = nullptr;

    TrustedKey(fs::path path, EVP_PKEY *value) : Path(std::move(path)), Value(value) {}
    TrustedKey(const TrustedKey &) = delete;
    TrustedKey &operator=(const TrustedKey &) = delete;
    TrustedKey(TrustedKey &&other) noexcept : Path(std::move(other.Path)), Value(other.Value) { other.Value = nullptr; }
    TrustedKey &operator=(TrustedKey &&other) noexcept
    {
        if (this == &other) return *this;
        if (Value) EVP_PKEY_free(Value);
        Path = std::move(other.Path);
        Value = other.Value;
        other.Value = nullptr;
        return *this;
    }
    ~TrustedKey()
    {
        if (Value) EVP_PKEY_free(Value);
    }
};

std::string SafeFilenamePart(std::string value)
{
    for (char &character : value)
        if (!std::isalnum(static_cast<unsigned char>(character)) && character != '-' && character != '_') character = '_';
    return value.empty() ? "unnamed" : value;
}

std::vector<unsigned char> ReadBinaryFile(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string Sha256File(const std::filesystem::path &path)
{
    auto data = ReadBinaryFile(path);
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(data.data(), data.size(), hash);
    std::ostringstream out;
    for (unsigned char c : hash)
        out << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c);
    return out.str();
}

bool VerifySignature(const std::filesystem::path &payloadPath, const std::filesystem::path &sigPath, EVP_PKEY *publicKey)
{
    auto payload = ReadBinaryFile(payloadPath);
    auto sig = ReadBinaryFile(sigPath);
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) return false;
    int ok = EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, publicKey);
    if (ok == 1) ok = EVP_DigestVerify(ctx, sig.data(), sig.size(), payload.data(), payload.size());
    EVP_MD_CTX_free(ctx);
    return ok == 1;
}

fs::path DefaultTrustStore(const fs::path &pluginRoot)
{
    if (const char *configured = std::getenv("CASCADE_PLUGIN_TRUST_STORE"); configured && *configured) return configured;
    fs::path prefix = pluginRoot;
    for (int level = 0; level < 3 && prefix.has_parent_path(); ++level)
        prefix = prefix.parent_path();
    return prefix / "share" / "cascade" / "trusted_keys";
}

fs::path RuntimePrefix()
{
    if (const char *configured = std::getenv("CASCADE_PREFIX"); configured && *configured) return configured;
    Dl_info libraryInfo{};
    if (dladdr(reinterpret_cast<void *>(&RuntimePrefix), &libraryInfo) != 0 && libraryInfo.dli_fname)
        return fs::weakly_canonical(libraryInfo.dli_fname).parent_path().parent_path();
    const char *home = std::getenv("HOME");
    return home && *home ? fs::path(home) / ".local" : fs::path(".");
}

std::vector<TrustedKey> LoadTrustedKeys(const fs::path &trustStore, bool warnIfMissing)
{
    std::vector<TrustedKey> keys;
    if (!fs::is_directory(trustStore))
    {
        if (warnIfMissing) LOG_WARN("PLUGIN", "Plugin trust store not found: " << trustStore.string());
        return keys;
    }
    std::vector<fs::path> paths;
    for (const auto &entry : fs::directory_iterator(trustStore))
        if (entry.is_regular_file() && entry.path().extension() == ".pem") paths.push_back(entry.path());
    std::sort(paths.begin(), paths.end());

    for (const auto &path : paths)
    {
        FILE *file = fopen(path.string().c_str(), "r");
        if (!file)
        {
            LOG_WARN("PLUGIN", "Cannot open trusted plugin key: " << path.string());
            continue;
        }
        EVP_PKEY *key = PEM_read_PUBKEY(file, nullptr, nullptr, nullptr);
        fclose(file);
        if (!key)
        {
            LOG_WARN("PLUGIN", "Cannot parse trusted plugin key: " << path.string());
            continue;
        }
        keys.emplace_back(path, key);
    }
    return keys;
}

const TrustedKey *VerifyWithTrustedKey(const fs::path &manifest, const fs::path &signature, const std::vector<TrustedKey> &keys)
{
    for (const auto &key : keys)
        if (VerifySignature(manifest, signature, key.Value)) return &key;
    return nullptr;
}

bool IsContainedPath(const fs::path &root, const fs::path &candidate)
{
    const fs::path canonicalRoot = fs::weakly_canonical(root);
    const fs::path canonicalCandidate = fs::weakly_canonical(candidate);
    const fs::path relative = canonicalCandidate.lexically_relative(canonicalRoot);
    return !relative.empty() && *relative.begin() != "..";
}

std::vector<fs::path> PackageDirectories(const fs::path &pluginRoot)
{
    std::vector<fs::path> packages;
    if (!fs::is_directory(pluginRoot)) return packages;
    for (const auto &entry : fs::directory_iterator(pluginRoot))
        if (entry.is_directory()) packages.push_back(entry.path());
    std::sort(packages.begin(), packages.end());
    return packages;
}

struct VerifiedCppPlugin
{
    fs::path Path;
    PluginOrigin Origin;
};

std::vector<VerifiedCppPlugin> VerifiedCppPluginPaths(const fs::path &packageDir, const std::vector<TrustedKey> &keys,
                                                       PluginTrustPolicy policy)
{
    fs::path manifestPath = packageDir / "plugin_manifest.json";
    fs::path sigPath = packageDir / "plugin_manifest.json.sig";
    if (!fs::is_regular_file(manifestPath))
    {
        LOG_WARN("PLUGIN", "Plugin manifest missing in package " << packageDir.string());
        return {};
    }

    try
    {
        const bool hasSignature = fs::is_regular_file(sigPath);
        const TrustedKey *trustedKey = hasSignature ? VerifyWithTrustedKey(manifestPath, sigPath, keys) : nullptr;
        if (policy == PluginTrustPolicy::RequireSigned && !trustedKey)
        {
            LOG_WARN("PLUGIN", "Plugin package requires a trusted signature: " << manifestPath.string());
            return {};
        }
        if (hasSignature && !trustedKey)
            LOG_WARN("PLUGIN", "Plugin signature is not trusted; loading as verified: " << manifestPath.string());

        std::ifstream in(manifestPath);
        nlohmann::json manifest;
        in >> manifest;
        if (manifest.value("schema", 0) != 2)
            throw std::runtime_error("unsupported manifest schema");
        if (manifest.value("package", "") != packageDir.filename().string())
            throw std::runtime_error("manifest package name does not match its directory");

        PluginOrigin packageOrigin;
        packageOrigin.Package = packageDir.filename().string();
        packageOrigin.ManifestPath = fs::weakly_canonical(manifestPath).string();
        packageOrigin.ManifestSha256 = Sha256File(manifestPath);
        packageOrigin.Trust = trustedKey ? PluginTrustStatus::Signed : PluginTrustStatus::Verified;
        if (trustedKey) packageOrigin.SignerFingerprint = Sha256File(trustedKey->Path);
        LOG_DEBUG("PLUGIN", ToString(packageOrigin.Trust) << " package " << packageOrigin.Package);
        std::vector<VerifiedCppPlugin> result;
        for (const auto &entry : manifest.value("modules", nlohmann::json::array()))
        {
            if (entry.value("language", "") != "cpp") continue;
            fs::path rel = entry.value("path", "");
            if (rel.empty() || rel.is_absolute())
            {
                LOG_WARN("PLUGIN", "Ignoring invalid manifest path in " << manifestPath.string() << ": " << rel.string());
                continue;
            }
            fs::path full = packageDir / rel;
            if (!fs::is_regular_file(full) || !IsContainedPath(packageDir, full))
            {
                LOG_WARN("PLUGIN", "Manifest plugin path is missing or escapes its package: " << full.string());
                continue;
            }
            std::string filename = full.filename().string();
            if (filename.size() < 9 || filename.rfind("Module.so") != filename.size() - 9)
            {
                LOG_WARN("PLUGIN", "Ignoring C++ plugin whose filename does not end with Module.so: " << full.string());
                continue;
            }
            std::string expected = entry.value("sha256", "");
            std::string actual = Sha256File(full);
            if (expected.empty() || actual != expected)
            {
                LOG_WARN("PLUGIN", "Plugin hash mismatch: " << full.string());
                continue;
            }
            auto origin = packageOrigin;
            origin.ArtifactSha256 = actual;
            result.push_back({full, std::move(origin)});
        }
        std::sort(result.begin(), result.end(),
                  [](const VerifiedCppPlugin &left, const VerifiedCppPlugin &right) { return left.Path < right.Path; });
        return result;
    }
    catch (const std::exception &e)
    {
        LOG_WARN("PLUGIN", "Failed to verify plugin manifest " << manifestPath.string() << ": " << e.what());
        return {};
    }
}

struct IsolatedRunHeader
{
    std::uint32_t Magic = 0x43534344;
    std::int32_t Status = static_cast<std::int32_t>(ModuleStatus::Failed);
    std::int32_t Phase = static_cast<std::int32_t>(ModulePhase::Execute);
    std::uint32_t MessageSize = 0;
};

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
} // namespace

AMCM::AMCM() : AMCM(PluginTrustPolicy::Verified) {}

AMCM::AMCM(PluginTrustPolicy trustPolicy) : m_TrustPolicy(trustPolicy)
{
    InterruptManager::Init();
    m_Dag = std::make_unique<DAGManager>();
    const char *pluginDir = std::getenv("CASCADE_PLUGIN_DIR");
    if (!pluginDir || std::string(pluginDir).empty())
        LoadPlugins((RuntimePrefix() / "lib" / "cascade" / "plugin").string());
    else
    {
        LoadPlugins(pluginDir);
    }
}

std::shared_ptr<IAnalysisModule> AMCM::RegisterModule(const std::string &base, const std::string &instanceName)
{
    if (instanceName.empty()) throw std::invalid_argument("Module instance name cannot be empty");
    const auto origin = AnalysisModuleRegistry::Get().GetPluginOrigin(base);
    if (m_TrustPolicy == PluginTrustPolicy::RequireSigned && origin && origin->Trust != PluginTrustStatus::Signed)
        throw std::runtime_error("Module requires a signed plugin under the active trust policy: " + base);
    auto mod = AnalysisModuleRegistry::Get().Create(base);
    if (origin && mod->BaseName() != base) AnalysisModuleRegistry::Get().SetPluginOrigin(mod->BaseName(), *origin);
    mod->SetName(instanceName);
    auto ptr = std::shared_ptr<IAnalysisModule>(std::move(mod));
    std::lock_guard<std::recursive_mutex> registrationLock(m_RegistrationMutex);
    {
        std::lock_guard<std::mutex> lock(m_ControlMutex);
        if (m_Modules.count(instanceName)) throw std::runtime_error("Module instance already registered: " + instanceName);
    }
    {
        std::lock_guard<std::mutex> lock(m_ControlMutex);
        m_Modules[instanceName] = ptr;
    }
    LOG_INFO("CONTROL", "Module " << base << " is registered as " << instanceName);
    return ptr;
}

std::vector<std::string> AMCM::ListAvailableModules() const
{
    auto modules = AnalysisModuleRegistry::Get().ListModules();
    if (m_TrustPolicy != PluginTrustPolicy::RequireSigned) return modules;
    modules.erase(std::remove_if(modules.begin(), modules.end(),
                                 [](const std::string &name)
                                 {
                                     const auto origin = AnalysisModuleRegistry::Get().GetPluginOrigin(name);
                                     return origin && origin->Trust != PluginTrustStatus::Signed;
                                 }),
                  modules.end());
    return modules;
}

std::vector<ModuleMetadata> AMCM::ListAvailableModuleMetadata() const
{
    auto metadata = AnalysisModuleRegistry::Get().ListModuleMetadata();
    if (m_TrustPolicy != PluginTrustPolicy::RequireSigned) return metadata;
    metadata.erase(std::remove_if(metadata.begin(), metadata.end(),
                                  [](const ModuleMetadata &item)
                                  {
                                      const auto origin = AnalysisModuleRegistry::Get().GetPluginOrigin(item.Name);
                                      return origin && origin->Trust != PluginTrustStatus::Signed;
                                  }),
                   metadata.end());
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
    RunResult result = mod->Run();
    RecordRun_(mod, result);
    LOG_INFO("CONTROL", "Module " << name << " finished execution with status " << ToString(result.Status));
    return result;
}

RunResult AMCM::RunAModule(std::shared_ptr<IAnalysisModule> mod)
{
    mod = ValidateModuleHandle_(mod);
    LOG_INFO("CONTROL", "Running module " << mod->Name());
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
    auto module = RegisteredModule_(name);

    LOG_INFO("CONTROL", "Running module " << name << " in a subprocess");
    module->PrepareExternalRun();

    int channel[2];
    if (pipe(channel) != 0)
    {
        const std::string message = "Cannot create isolated execution channel: " + std::string(std::strerror(errno));
        RunResult result = module->AdoptExternalRunResult(ExternalFailure(ModuleStatus::Failed, message));
        RecordRun_(module, result);
        return result;
    }

    const pid_t child = fork();
    if (child < 0)
    {
        const std::string message = "Cannot fork isolated module: " + std::string(std::strerror(errno));
        close(channel[0]);
        close(channel[1]);
        RunResult result = module->AdoptExternalRunResult(ExternalFailure(ModuleStatus::Failed, message));
        RecordRun_(module, result);
        return result;
    }

    if (child == 0)
    {
        close(channel[0]);
        signal(SIGSEGV, SIG_DFL);
        signal(SIGABRT, SIG_DFL);
        signal(SIGBUS, SIG_DFL);
        signal(SIGILL, SIG_DFL);
        signal(SIGFPE, SIG_DFL);
        RunResult childResult;
        try
        {
            childResult = module->RunPreparedExternal();
        }
        catch (const std::exception &error)
        {
            childResult = ExternalFailure(ModuleStatus::Failed, error.what());
        }
        catch (...)
        {
            childResult = ExternalFailure(ModuleStatus::Failed, "Unknown exception escaped isolated module runner");
        }
        if (childResult.Message.size() > 4096) childResult.Message.resize(4096);
        IsolatedRunHeader header;
        header.Status = static_cast<std::int32_t>(childResult.Status);
        header.Phase = static_cast<std::int32_t>(childResult.Phase);
        header.MessageSize = static_cast<std::uint32_t>(childResult.Message.size());
        const bool written = WriteAll(channel[1], &header, sizeof(header)) &&
                             WriteAll(channel[1], childResult.Message.data(), childResult.Message.size());
        close(channel[1]);
        _exit(written ? 0 : 125);
    }

    close(channel[1]);
    int childStatus = 0;
    bool cancellationSent = false;
    int cancellationPolls = 0;
    while (true)
    {
        const pid_t waited = waitpid(child, &childStatus, WNOHANG);
        if (waited == child) break;
        if (waited < 0 && errno != EINTR)
        {
            childStatus = 0;
            break;
        }
        if (module->IsCancellationRequested())
        {
            if (!cancellationSent)
            {
                kill(child, SIGTERM);
                cancellationSent = true;
            }
            else if (++cancellationPolls >= 50)
            {
                kill(child, SIGKILL);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    RunResult externalResult;
    if (cancellationSent)
    {
        externalResult = ExternalFailure(ModuleStatus::Interrupted, "Isolated module was cancelled");
    }
    else if (WIFSIGNALED(childStatus))
    {
        externalResult = ExternalFailure(ModuleStatus::Failed,
                                         "Isolated module terminated by signal " + std::to_string(WTERMSIG(childStatus)));
    }
    else
    {
        IsolatedRunHeader header;
        if (!ReadAll(channel[0], &header, sizeof(header)) || header.Magic != 0x43534344 || header.MessageSize > 4096)
        {
            externalResult = ExternalFailure(ModuleStatus::Failed, "Isolated module exited without a valid result");
        }
        else
        {
            std::string message(header.MessageSize, '\0');
            if (!ReadAll(channel[0], message.data(), message.size()))
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
            }
        }
    }
    close(channel[0]);

    RunResult result = module->AdoptExternalRunResult(std::move(externalResult));
    RecordRun_(module, result);
    LOG_INFO("CONTROL", "Isolated module " << name << " finished with status " << ToString(result.Status));
    return result;
}

RunResult AMCM::RunAModuleIsolated(std::shared_ptr<IAnalysisModule> module)
{
    module = ValidateModuleHandle_(module);
    return RunAModuleIsolated(module->Name());
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
    RegisteredModule_(name);
    m_Dag->AddNode(
        name, dependencies,
        [this, name, isolated]()
        {
            const RunResult result = isolated ? RunAModuleIsolated(name) : RunAModule(name);
            if (!result.AllowsDependents())
                throw std::runtime_error("Module " + name + " finished with status " + ToString(result.Status) +
                                         (result.Message.empty() ? std::string() : ": " + result.Message));
        });
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
    workflow.Runtime = ProvenanceRecorder::Runtime("cpp");
    workflow.FailFast = failFast;
    workflow.Succeeded = true;

    std::map<std::string, ModuleRunManifest> manifestsByInstance;
    for (const auto &entry : m_ExecutedModules)
    {
        std::optional<ModuleRunManifest> manifest = ProvenanceRecorder::FindModuleRun(entry.RunId);
        if (!manifest && !entry.ManifestPath.empty() && std::filesystem::is_regular_file(entry.ManifestPath))
            manifest = ProvenanceRecorder::LoadModuleRun(entry.ManifestPath);
        if (manifest)
        {
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
    namespace fs = std::filesystem;
    static std::mutex pluginLoadMutex;
    static std::set<std::string> loadedPlugins;
    const fs::path pluginRoot(path);
    if (!fs::is_directory(pluginRoot))
    {
        LOG_WARN("CONTROL", "Plugin directory not found: " << path);
        return;
    }
    const fs::path trustStore = DefaultTrustStore(pluginRoot);
    const auto trustedKeys = LoadTrustedKeys(trustStore, m_TrustPolicy == PluginTrustPolicy::RequireSigned);

    for (const auto &packageDir : PackageDirectories(pluginRoot))
    {
        for (const auto &plugin : VerifiedCppPluginPaths(packageDir, trustedKeys, m_TrustPolicy))
        {
            std::lock_guard<std::mutex> loadLock(pluginLoadMutex);
            const auto &pluginFile = plugin.Path;
            const std::string canonicalPlugin = fs::weakly_canonical(pluginFile).string();
            if (loadedPlugins.count(canonicalPlugin))
            {
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
            void *handle = dlopen(pluginFile.c_str(), RTLD_NOW);
            if (!handle) LOG_WARN("PLUGIN", "dlopen failed for '" << pluginFile.string() << "': " << dlerror());
            if (!handle) continue;
            if (AnalysisModuleRegistry::Get().ListModules() != modulesBeforeLoad)
            {
                LOG_ERROR("PLUGIN", "Plugin performed static module registration before ABI validation: " << pluginFile.string());
                rollbackRegistrations();
                dlclose(handle);
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
                continue;
            }

            const int abi = abiFn();
            if (abi != CASCADE_PLUGIN_ABI_VERSION)
            {
                LOG_ERROR("PLUGIN", "Plugin ABI mismatch for '" << pluginFile.string() << "': " << abi << " != " << CASCADE_PLUGIN_ABI_VERSION);
                rollbackRegistrations();
                dlclose(handle);
                continue;
            }
            const char *rawTag = abiTagFn();
            if (!rawTag || std::string(rawTag) != CASCADE_ABI_TAG)
            {
                LOG_ERROR("PLUGIN", "Plugin ABI tag mismatch for '" << pluginFile.string() << "'");
                rollbackRegistrations();
                dlclose(handle);
                continue;
            }

            try
            {
                regFn();
                const auto modulesAfterLoad = AnalysisModuleRegistry::Get().ListModules();
                if (modulesAfterLoad == modulesBeforeLoad)
                    throw std::runtime_error("plugin registration did not add a module");
                for (const auto &name : modulesAfterLoad)
                    if (!moduleSetBeforeLoad.count(name)) AnalysisModuleRegistry::Get().SetPluginOrigin(name, plugin.Origin);
                loadedPlugins.insert(canonicalPlugin);
                LOG_INFO("PLUGIN", "Loaded " << ToString(plugin.Origin.Trust) << " plugin " << pluginFile.string());
            }
            catch (const std::exception &error)
            {
                LOG_ERROR("PLUGIN", "Plugin registration failed for '" << pluginFile.string() << "': " << error.what());
                rollbackRegistrations();
                dlclose(handle);
            }
            catch (...)
            {
                LOG_ERROR("PLUGIN", "Plugin registration failed for '" << pluginFile.string() << "' with an unknown exception");
                rollbackRegistrations();
                dlclose(handle);
            }
        }
    }
}
