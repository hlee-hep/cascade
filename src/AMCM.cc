#include "AMCM.hh"
#include "AnalysisModuleRegistry.hh"
#include "InterruptManager.hh"
#include "Logger.hh"
#include "PluginABI.hh"
#include <chrono>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <thread>
#include <yaml-cpp/yaml.h>
namespace py = pybind11;

AMCM::AMCM()
{
    InterruptManager::Init();
    m_Dag = std::make_unique<DAGManager>();
    const char *pluginDir = std::getenv("CASCADE_PLUGIN_DIR");
    if (!pluginDir || std::string(pluginDir).empty())
    {
        const char *home = std::getenv("HOME");
        if (home && std::string(home).size() > 0)
            LoadPlugins(std::string(home) + "/.local/lib/cascade/plugin");
    }
    else
    {
        LoadPlugins(pluginDir);
    }
}

std::shared_ptr<IAnalysisModule> AMCM::RegisterModule(const std::string &base, const std::string &instanceName)
{
    auto mod = AnalysisModuleRegistry::Get().Create(base);
    mod->SetName(instanceName);
    LOG_DEBUG("CONTROL", "mod ptr before shared_ptr: " << mod.get());
    auto ptr = std::shared_ptr<IAnalysisModule>(std::move(mod));
    LOG_DEBUG("CONTROL", "shared_ptr made");
    m_Modules[instanceName] = ptr;
    LOG_DEBUG("CONTROL", "mod ptr = " << ptr.get() << ", &param manager = " << &ptr->GetParamManager());
    LOG_INFO("CONTROL", "Module " << base << " is registered as " << instanceName);
    m_Dag->SetParamManagerMap({{instanceName, &ptr->GetParamManager()}});
    return ptr;
}

std::shared_ptr<IAnalysisModule> AMCM::RegisterModule(const std::string &base)
{
    int count = ++m_ModuleNameCounter[base];
    std::string autoName = base + "_" + std::to_string(count);
    return RegisterModule(base, autoName);
}

std::vector<std::string> AMCM::ListRegisteredModules() const
{
    std::vector<std::string> names = {};
    for (auto &[_, mod] : m_Modules)
        names.push_back(mod->Name());

    return names;
}

std::shared_ptr<IAnalysisModule> AMCM::GetModule(const std::string &name)
{
    auto it = m_Modules.find(name);

    if (it == m_Modules.end())
    {
        LOG_ERROR("CONTROL", "Cannot found " << name << " module in the registry...");
        return nullptr;
    }
    else
    {
        auto mod = it->second;
        return mod;
    }
}

std::string AMCM::GetStatus(const std::string &name) const
{
    std::lock_guard<std::mutex> lock(m_StatusMutex);
    if (m_Modules.count(name) == 0) throw std::runtime_error("Module not found");
    return m_Modules.at(name)->GetStatus();
}

std::map<std::string, std::map<std::string, double>> AMCM::GetAllProgress() const
{
    std::map<std::string, std::map<std::string, double>> result;
    for (const auto &[modName, mod] : m_Modules)
    {
        for (const auto &[mgrName, mgr] : mod->GetAllManagers())
        {
            result[modName][mgrName] = mgr->GetProgress();
        }
    }
    return result;
}

void AMCM::RunAModule(const std::string &name)
{
    py::gil_scoped_release release;
    auto it = m_Modules.find(name);
    if (it == m_Modules.end())
    {
        LOG_ERROR("CONTROL", "Cannot found " << name << " module in the registry...");
    }
    else
    {
        auto mod = it->second;
        LOG_INFO("CONTROL", "Running module " << name);
        mod->Run();
        m_ExecutedModules.push_back(mod);
        LOG_INFO("CONTROL", "Module " << name << " finished execution");
    }
}

void AMCM::RunAModule(std::shared_ptr<IAnalysisModule> mod) { RunAModule(mod->Name()); }

void AMCM::SequentialRun()
{
    LOG_INFO("CONTROL", "Sequential Run is starting.");
    for (auto &[_, mod] : m_Modules)
        RunAModule(mod);
    LOG_INFO("CONTROL", "Sequential Run is ended.");
}

void AMCM::RunModules(const std::vector<std::string> &group)
{
    LOG_INFO("CONTROL", "Running " << group.size() << " modules from provided list");
    for (const auto &name : group)
        RunAModule(name);
    LOG_INFO("CONTROL", "Finished running provided module list");
}

void AMCM::RunModules(std::vector<std::shared_ptr<IAnalysisModule>> group)
{
    LOG_INFO("CONTROL", "Running " << group.size() << " provided module handles");
    for (const auto &mod : group)
        RunAModule(mod);
    LOG_INFO("CONTROL", "Finished running provided module handles");
}

void AMCM::RunDAG()
{
    LOG_INFO("CONTROL", "Executing DAG workflow");
    m_Dag->Execute();
    LOG_INFO("CONTROL", "DAG workflow execution completed");
}

void AMCM::SaveRunLog() const
{
    // DEPRECATED//
    std::time_t t = std::time(nullptr);
    std::tm *now = std::localtime(&t);
    char buf[20];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", now);
    std::string filename = "control_log_" + std::string(buf);
    int counter = 0;

    YAML::Emitter out;
    out << YAML::BeginMap;
    out << YAML::Key << "modules" << YAML::Value << YAML::BeginSeq;

    for (const auto &mod : m_ExecutedModules)
    {
        std::string name = mod->Name();
        std::string baseName = mod->BaseName();
        std::string codeHash = mod->GetCodeHash();
        std::string status = mod->GetStatus();
        auto &params = mod->GetParamManager();
        out << YAML::BeginMap;
        out << YAML::Key << "name" << YAML::Value << name;
        out << YAML::Key << "module" << YAML::Value << baseName;
        out << YAML::Key << "codehash" << YAML::Value << codeHash;
        out << YAML::Key << "status" << YAML::Value << status;
        out << YAML::Key << "params" << YAML::Value << params.ToYAMLNode();
        out << YAML::EndMap;

        if (counter++ < 5) filename += ("_" + name);
    }

    out << YAML::EndSeq;
    out << YAML::EndMap;

    filename += ".yaml";
    std::filesystem::create_directories("run_logs");
    std::ofstream fout("run_logs/" + filename);
    fout << out.c_str();
    LOG_INFO("CONTROL", "Run log '" << filename << "' is saved.");
}

void AMCM::LoadPlugins(const std::string &path)
{
    namespace fs = std::filesystem;
    if (!fs::exists(path))
    {
        LOG_WARN("CONTROL", "Plugin directory not found: " << path);
        return;
    }
    fs::path keyPath = fs::path(path) / "plugin_pubkey.pem";
    bool verifySignatures = fs::exists(keyPath);
    EVP_PKEY *publicKey = nullptr;
    if (verifySignatures)
    {
        FILE *keyFile = fopen(keyPath.string().c_str(), "r");
        if (!keyFile)
        {
            LOG_ERROR("CONTROL", "Failed to open plugin public key: " << keyPath.string());
            return;
        }
        publicKey = PEM_read_PUBKEY(keyFile, nullptr, nullptr, nullptr);
        fclose(keyFile);
        if (!publicKey)
        {
            LOG_ERROR("CONTROL", "Failed to read plugin public key: " << keyPath.string());
            return;
        }
        LOG_INFO("CONTROL", "Plugin signature verification enabled with " << keyPath.string());
    }
    else
    {
        LOG_ERROR("CONTROL", "Plugin public key not found; skipping plugin load: " << keyPath.string());
        return;
    }

    for (auto &p : fs::directory_iterator(path))
    {
        if (p.path().extension() == ".so")
        {
            std::string filename = p.path().filename().string();
            if (filename.size() < 9 || filename.rfind("Module.so") != filename.size() - 9)
            {
                continue;
            }
            if (verifySignatures)
            {
                fs::path sigPath = p.path();
                sigPath += ".sig";
                if (!fs::exists(sigPath))
                {
                    LOG_ERROR("CONTROL", "Missing plugin signature: " << sigPath.string());
                    continue;
                }

                std::ifstream bin(p.path(), std::ios::binary);
                std::ifstream sig(sigPath, std::ios::binary);
                if (!bin || !sig)
                {
                    LOG_ERROR("CONTROL", "Failed to read plugin or signature: " << p.path().string());
                    continue;
                }

                std::vector<unsigned char> data((std::istreambuf_iterator<char>(bin)), std::istreambuf_iterator<char>());
                std::vector<unsigned char> sigData((std::istreambuf_iterator<char>(sig)), std::istreambuf_iterator<char>());

                EVP_MD_CTX *ctx = EVP_MD_CTX_new();
                if (!ctx)
                {
                    LOG_ERROR("CONTROL", "Failed to create EVP_MD_CTX for signature verification");
                    continue;
                }

                int ok = EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, publicKey);
                if (ok != 1)
                {
                    LOG_ERROR("CONTROL", "Signature verify init failed for " << p.path().string());
                    EVP_MD_CTX_free(ctx);
                    continue;
                }
                ok = EVP_DigestVerify(ctx, sigData.data(), sigData.size(), data.data(), data.size());
                EVP_MD_CTX_free(ctx);
                if (ok != 1)
                {
                    LOG_ERROR("CONTROL", "Plugin signature invalid: " << p.path().string());
                    continue;
                }
            }
            void *handle = dlopen(p.path().c_str(), RTLD_NOW);
            if (!handle) LOG_ERROR("CONTROL", "dlopen failed for '" << p.path().string() << "': " << dlerror());
            if (!handle) continue;
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

            if (abiFn)
            {
                int abi = abiFn();
                if (abi != CASCADE_PLUGIN_ABI_VERSION)
                {
                    LOG_ERROR("CONTROL", "Plugin ABI mismatch for '" << p.path().string() << "': " << abi << " != " << CASCADE_PLUGIN_ABI_VERSION);
                    dlclose(handle);
                    continue;
                }
                if (abiTagFn)
                {
                    std::string tag = abiTagFn();
                    if (tag != CASCADE_ABI_TAG)
                    {
                        LOG_ERROR("CONTROL", "Plugin ABI tag mismatch for '" << p.path().string() << "'");
                        dlclose(handle);
                        continue;
                    }
                }
                else
                {
                    LOG_WARN("CONTROL", "Plugin '" << p.path().string() << "' missing ABI tag; skipping strict tag check");
                }
                if (regFn)
                {
                    regFn();
                }
                else
                {
                    LOG_WARN("CONTROL", "Plugin '" << p.path().string() << "' provides ABI version but no CascadeRegisterPlugin()");
                }
            }
            else if (regFn)
            {
                LOG_WARN("CONTROL", "Plugin '" << p.path().string() << "' has no ABI version; calling CascadeRegisterPlugin()");
                regFn();
            }
            else
            {
                LOG_WARN("CONTROL", "Legacy plugin loaded without ABI entry points: " << p.path().string());
            }
        }
    }
    if (publicKey) EVP_PKEY_free(publicKey);
}
