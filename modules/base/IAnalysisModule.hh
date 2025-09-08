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
        if (!RunCheck())
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
            CacheManager::AddHash(basename, _hash);
            SetStatus("Done");
        }
        else
            SetStatus("Interrupted");
    }

    virtual void Description() const = 0;

    std::string GetParamsToJSON() { return _param.DumpJSON(); }

    void SetName(const std::string &name) { m_name = name; }
    std::string Name() const { return m_name; }
    std::string BaseName() const { return basename; }
    std::string GetCodeHash() const { return code_version_hash; }

    void SetParamFromPy(const std::string &key, py::object val) { _param.SetParamFromPy(key, val); }
    void SetParamsFromDict(const py::dict &d)
    {
        for (auto item : d)
            SetParamFromPy(py::str(item.first), py::reinterpret_borrow<py::object>(item.second));
    }
    void SetParamsFromYAML(const std::string &yamlPath)
    {
        const YAML::Node node = YAML::LoadFile(yamlPath.c_str());
        _param.SetParamsFromYAML(node);
    }
    void DumpParamsToYAML(const std::string &filepath) const
    {
        YAML::Emitter out;
        out.SetIndent(4);
        out.SetMapFormat(YAML::Block);
        out.SetSeqFormat(YAML::Flow);
        out << _param.ToYAMLNode();
        std::ofstream fout(filepath);
        fout << out.c_str();
    }
    std::string GetStatus() const { return _status; }
    ParamManager &GetParamManager() { return _param; }
    const auto &GetAllManagers() const { return _mgr; }
    void SetStatus(const std::string &s)
    {
        _status = s;
        LOG_INFO(Name(), "Status : " << s);
    }

  protected:
    virtual void Init() = 0;
    virtual void Execute() = 0;
    virtual void Finalize() = 0;

    void RegisterAnalysisManager(const std::string &name = "main")
    {
        auto it = _mgr.find(name);
        if (it != _mgr.end())
            LOG_ERROR(Name(), "The same name of analysis manager exists.");
        else
            _mgr[name] = std::make_unique<AnalysisManager>();
    }

    AnalysisManager *GetAnalysisManager(const std::string &name) const
    {
        auto it = _mgr.find(name);
        return it != _mgr.end() ? it->second.get() : nullptr;
    }

    AnalysisManager *am(const std::string &name = "main") const { return GetAnalysisManager(name); }

    std::string _status = "Pending";
    ParamManager _param;
    std::string _hash;

    std::string basename = "Interface";
    std::string m_name = "";
    std::string code_version_hash = "";

  private:
    std::map<std::string, std::unique_ptr<AnalysisManager>> _mgr;

    std::string ComputeSnapshotHash() const { return SnapshotHasher::Compute(_param, GetAllManagers(), basename, code_version_hash); }

    bool RunCheck()
    {
        if (_param.Get<bool>("dry_run"))
        {
            LOG_INFO(Name(), "DRY run is enabled. variables and setting will be shown.");
            for (auto &[_, mg] : _mgr)
            {
                mg->PrintConfig();
                mg->PrintHists();
                mg->PrintCuts();
            }
            LOG_INFO("ParamManager", _param.PrintParameters());
            return false;
        }
        if (_param.Get<bool>("force_run"))
        {
            LOG_INFO(Name(), "Force run is enabled. Run will be started.");
            return true;
        }

        _hash = ComputeSnapshotHash();
        bool dupl = CacheManager::IsHashCached(basename, _hash);
        if (dupl) LOG_ERROR(Name(), "Duplication of hash is detected.");
        return !dupl;
    }
};
