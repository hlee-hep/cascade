#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "AMCM.hh"
#include "Provenance.hh"
#include "RuntimeOptions.hh"
#include "AnalysisModuleRegistry.hh"
#include "InterruptManager.hh"
#include "ExecutionResources.hh"
#include "Logger.hh"
#include "PluginABI.hh"
#include "PluginPaths.hh"
#include "PluginVerifier.hh"
#include <algorithm>
#include <cmath>
#include <dlfcn.h>
#include <filesystem>
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
#include <yaml-cpp/yaml.h>
#include <vector>

namespace
{
namespace fs = std::filesystem;

std::vector<fs::path> CppPluginRoots()
{
    std::vector<fs::path> roots;
    for (const auto &root : PluginPaths::Roots("cpp")) roots.emplace_back(root);
    return roots;
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

AMCM::AMCM() : AMCM(PluginTrustPolicy::Verified, true, ::GetRuntimeOptions()) {}

AMCM::AMCM(PluginTrustPolicy trustPolicy) : AMCM(trustPolicy, true, ::GetRuntimeOptions()) {}

AMCM::AMCM(PluginTrustPolicy trustPolicy, bool discoverPlugins)
    : AMCM(trustPolicy, discoverPlugins, ::GetRuntimeOptions())
{
}

AMCM::AMCM(PluginTrustPolicy trustPolicy, bool discoverPlugins, RuntimeOptions runtimeOptions)
    : m_TrustPolicy(trustPolicy), m_RuntimeOptions(std::move(runtimeOptions)), m_IndexPlugins(discoverPlugins)
{
    InterruptManager::Init();
    m_Dag = std::make_unique<DAGManager>(m_RuntimeOptions);
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
        mod->SetCodeHash("code-sha256:" +
                         (origin->CodeSha256.empty() ? origin->ArtifactSha256 : origin->CodeSha256));
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
    RuntimeOptionsScope runtimeScope(m_RuntimeOptions);
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
    RuntimeOptionsScope runtimeScope(m_RuntimeOptions);
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
