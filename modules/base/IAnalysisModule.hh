#pragma once
#include "AnalysisManager.hh"
#include "CacheManager.hh"
#include "InterruptManager.hh"
#include "Logger.hh"
#include "ParamManager.hh"
#include "SnapshotHasher.hh"
#include "sha256.hh"
#include <memory>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <string>
#include <vector>
#include <yaml-cpp/yaml.h>

namespace py = pybind11;

class AnalysisManager;

class IAnalysisModule
{
  public:
    virtual ~IAnalysisModule() = default;

    virtual void Run()
    {
        RegisterAnalysisManager("main");
        SetStatus("Initializing");
        Init();
        if (InterruptManager::IsInterrupted())
        {
            SetStatus("Interrupted");
            return;
        }
        if (!RunCheck_())
        {
            SetStatus("Skipped");
            return;
        }
        SetStatus("Running");
        try
        {
            Execute();
        }
        catch (const std::exception &e)
        {
            if (InterruptManager::IsInterrupted())
            {
                SetStatus("Interrupted");
                LOG_WARN(Name(), "Execution interrupted: " << e.what());
            }
            else
            {
                SetStatus("Failed");
                LOG_ERROR(Name(), "Module failed: " << e.what());
                throw;
            }
            return;
        }
        if (InterruptManager::IsInterrupted())
        {
            SetStatus("Interrupted");
            return;
        }
        SetStatus("Finalizing");
        Finalize();

        if (!InterruptManager::IsInterrupted())
        {
            CacheManager::AddHash(m_Basename, m_Hash);
            SetStatus("Done");
        }
        else
            SetStatus("Interrupted");
    }

    virtual void Description() const = 0;

    std::string GetParamsToJSON() { return m_Param.DumpJSON(); }

    void SetName(const std::string &name) { m_Name = name; }
    std::string Name() const { return m_Name; }
    std::string BaseName() const { return m_Basename; }
    std::string GetCodeHash() const { return m_CodeVersionHash; }

    void SetParamFromPy(const std::string &key, py::object val) { m_Param.SetParamFromPy(key, val); }
    void SetParamsFromDict(const py::dict &d) { m_Param.SetParamsFromDict(d); }
    void LoadParamsFromYAML(const std::string &path) { m_Param.LoadYAMLFile(path); }
    void LoadParamsFromJSON(const std::string &path) { m_Param.LoadJSONFile(path); }
    void SaveParamsToYAML(const std::string &path) { m_Param.SaveYAMLFile(path); }
    void SaveParamsToJSON(const std::string &path) { m_Param.SaveJSONFile(path); }
    std::string DumpParamsToYAML(int indent = 2) { return m_Param.DumpYAML(indent); }
    std::string DumpParamsToJSON(int indent = 4) { return m_Param.DumpJSON(indent); }

    std::string GetStatus() const { return m_Status; }
    ParamManager &GetParamManager() { return m_Param; }
    const auto &GetAllManagers() const { return m_Managers; }
    void SetStatus(const std::string &s)
    {
        m_Status = s;
        LOG_INFO(Name(), "Status : " << s);
    }

  protected:
    virtual void Init() = 0;
    virtual void Execute() = 0;
    virtual void Finalize() = 0;

    void RegisterAnalysisManager(const std::string &name = "main")
    {
        auto it = m_Managers.find(name);
        if (it != m_Managers.end())
            LOG_ERROR(Name(), "The same name of analysis manager exists.");
        else
            m_Managers[name] = std::make_unique<AnalysisManager>();
    }

    AnalysisManager *GetAnalysisManager(const std::string &name) const
    {
        auto it = m_Managers.find(name);
        return it != m_Managers.end() ? it->second.get() : nullptr;
    }

    AnalysisManager *Am(const std::string &name = "main") const { return GetAnalysisManager(name); }

    std::string m_Status = "Pending";
    ParamManager m_Param;
    std::string m_Hash;

    std::string m_Basename = "Interface";
    std::string m_Name = "";
    std::string m_CodeVersionHash = "";

  private:
    std::map<std::string, std::unique_ptr<AnalysisManager>> m_Managers;

    std::string ComputeSnapshotHash_() const { return SnapshotHasher::Compute(m_Param, GetAllManagers(), m_Basename, m_CodeVersionHash); }

    bool RunCheck_()
    {
        if (m_Param.Get<bool>("dry_run"))
        {
            LOG_INFO(Name(), "DRY run is enabled. variables and setting will be shown.");
            for (auto &[_, mg] : m_Managers)
            {
                mg->PrintConfig();
                mg->PrintHists();
                mg->PrintCuts();
            }
            LOG_INFO("ParamManager", m_Param.DumpJSON());
            return false;
        }
        if (m_Param.Get<bool>("force_run"))
        {
            LOG_INFO(Name(), "Force run is enabled. Run will be started.");
            return true;
        }

        m_Hash = ComputeSnapshotHash_();
        bool dupl = CacheManager::IsHashCached(m_Basename, m_Hash);
        if (dupl) LOG_ERROR(Name(), "Duplication of hash is detected.");
        return !dupl;
    }
};
