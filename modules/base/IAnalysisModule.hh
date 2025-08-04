#pragma once
#include "AnalysisManager.hh"
#include "CacheManager.hh"
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

    virtual void Description() const = 0;
    virtual void Init() = 0;
    virtual void Execute() = 0;
    virtual void Finalize() = 0;
    virtual std::string ComputeSnapshotHash() const = 0;

    virtual void SetName(const std::string &name) { m_name = name; }
    virtual std::string Name() const { return m_name; }
    virtual std::string BaseName() const { return basename; }
    virtual std::string GetCodeHash() const { return code_version_hash; }
    virtual void SetParamFromPy(const std::string &key, py::object val) { _param.SetParamFromAny(key, _param.ConvertFromPy(val)); }
    virtual void SetParamsFromDict(const py::dict &d)
    {
        for (auto item : d)
            SetParamFromPy(py::str(item.first), py::reinterpret_borrow<py::object>(item.second));
    }
    virtual void LoadParamsFromYAML(const YAML::Node &node)
    {
        for (const auto &it : node)
            _param.Set<std::string>(it.first.as<std::string>(), it.second.as<std::string>());
    }
    virtual std::string GetParametersAsJSON() const { return _param.DumpJSON(); }
    virtual std::string GetStatus() const { return _status; }
    virtual ParamManager &GetParamManager() { return _param; }
    const auto &GetAllManagers() const { return _mgr; }

  protected:
    virtual void RegisterAnalysisManager(const std::string &name = "main")
    {
        auto it = _mgr.find(name);
        if (it != _mgr.end())
            LOG_ERROR(Name(), "The same name of analysis manager exists.");
        else
            _mgr[name] = std::make_unique<AnalysisManager>();
    }
    virtual AnalysisManager *GetAnalysisManager(const std::string &name) const
    {
        auto it = _mgr.find(name);
        return it != _mgr.end() ? it->second.get() : nullptr;
    }

    AnalysisManager *am(const std::string &name = "main") const { return GetAnalysisManager(name); }
    virtual void SetStatus(const std::string &s)
    {
        _status = s;
        LOG_INFO(Name(), "Status : " << s);
    }
    std::string _status = "Pending";
    ParamManager _param;
    std::string _hash;

    std::string basename = "Interface";
    std::string m_name = "";
    std::string code_version_hash = "";

    virtual bool RunCheck()
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

  private:
    std::map<std::string, std::unique_ptr<AnalysisManager>> _mgr;
};
