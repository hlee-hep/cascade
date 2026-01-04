#include "AMCM.hh"
#include "AnalysisModuleRegistry.hh"
#include "InterruptManager.hh"
#include "Logger.hh"
#include <chrono>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <thread>
#include <yaml-cpp/yaml.h>
namespace py = pybind11;

AMCM::AMCM()
{
    InterruptManager::Init();
    m_Dag = std::make_unique<DAGManager>();
    LoadPlugins(std::string(std::getenv("HOME")) + "/.local/lib/cascade/plugin");
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
    for (auto &p : fs::directory_iterator(path))
    {
        if (p.path().extension() == ".so")
        {
            void *handle = dlopen(p.path().c_str(), RTLD_NOW);
            if (!handle) LOG_ERROR("CONTROL", "dlopen failed for '" << p.path().string() << "': " << dlerror());
        }
    }
}
