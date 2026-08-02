#pragma once
#include "ModuleMetadata.hh"
#include "PluginTrust.hh"
#include "Version.hh"
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class IAnalysisModule;

class AnalysisModuleRegistry
{
  public:
    using ModuleFactory = std::function<std::unique_ptr<IAnalysisModule>()>;
    using MetadataProvider = std::function<ModuleMetadata()>;
    static AnalysisModuleRegistry &Get();

    void Register(const std::string &name, ModuleFactory factory);
    void Register(const std::string &name, ModuleFactory factory, MetadataProvider metadata);
    void Unregister(const std::string &name);
    std::unique_ptr<IAnalysisModule> Create(const std::string &name) const;
    std::vector<std::string> ListModules() const;
    std::vector<ModuleMetadata> ListModuleMetadata() const;
    void SetPluginOrigin(const std::string &name, PluginOrigin origin);
    void ClearPluginOrigin(const std::string &name);
    std::optional<PluginOrigin> GetPluginOrigin(const std::string &name) const;

  private:
    mutable std::mutex m_Mutex;
    std::unordered_map<std::string, ModuleFactory> m_Factories;
    std::unordered_map<std::string, MetadataProvider> m_MetadataProviders;
    std::unordered_map<std::string, PluginOrigin> m_PluginOrigins;
};

template <typename Module>
void RegisterAnalysisModuleType(const std::string &name, AnalysisModuleRegistry::MetadataProvider metadata = {})
{
    if (!metadata)
    {
        metadata = [name]()
        {
            ModuleMetadata info;
            info.Name = name;
            info.Version = CascadeVersionString();
            return info;
        };
    }
    AnalysisModuleRegistry::Get().Register(
        name, []() -> std::unique_ptr<IAnalysisModule> { return std::make_unique<Module>(); }, std::move(metadata));
}
