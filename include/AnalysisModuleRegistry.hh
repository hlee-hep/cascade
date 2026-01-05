#pragma once
#include "ModuleMetadata.hh"
#include <functional>
#include <memory>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <string>
#include <unordered_map>
#include <vector>

class IAnalysisModule;

class AnalysisModuleRegistry
{
  public:
    using ModuleFactory = std::function<std::unique_ptr<IAnalysisModule>()>;
    using MetadataProvider = std::function<ModuleMetadata()>;
    using ModuleBinder = std::function<void(pybind11::module_ &)>;
    static AnalysisModuleRegistry &Get();

    void Register(const std::string &name, ModuleFactory factory);
    void Register(const std::string &name, ModuleFactory factory, MetadataProvider metadata);
    std::unique_ptr<IAnalysisModule> Create(const std::string &name) const;
    std::vector<std::string> ListModules() const;
    std::vector<ModuleMetadata> ListModuleMetadata() const;

  private:
    std::unordered_map<std::string, ModuleFactory> m_Factories;
    std::unordered_map<std::string, MetadataProvider> m_MetadataProviders;
};
