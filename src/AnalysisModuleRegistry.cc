#include "AnalysisModuleRegistry.hh"
#include "IAnalysisModule.hh"
#include "Logger.hh"
#include <algorithm>

AnalysisModuleRegistry &AnalysisModuleRegistry::Get()
{
    static AnalysisModuleRegistry instance;
    return instance;
}

void AnalysisModuleRegistry::Register(const std::string &name, ModuleFactory factory)
{
    Register(name, std::move(factory), {});
}

void AnalysisModuleRegistry::Register(const std::string &name, ModuleFactory factory, MetadataProvider metadata)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_Factories.count(name)) throw std::runtime_error("Duplicate module registration: " + name);
    m_Factories[name] = std::move(factory);
    if (metadata) m_MetadataProviders[name] = std::move(metadata);
}

void AnalysisModuleRegistry::Unregister(const std::string &name)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_PluginOrigins.erase(name);
    m_MetadataProviders.erase(name);
    m_Factories.erase(name);
}

void AnalysisModuleRegistry::SetPluginOrigin(const std::string &name, PluginOrigin origin)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_PluginOrigins[name] = std::move(origin);
}

void AnalysisModuleRegistry::ClearPluginOrigin(const std::string &name)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_PluginOrigins.erase(name);
}

std::optional<PluginOrigin> AnalysisModuleRegistry::GetPluginOrigin(const std::string &name) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    const auto iterator = m_PluginOrigins.find(name);
    if (iterator == m_PluginOrigins.end()) return std::nullopt;
    return iterator->second;
}

std::unique_ptr<IAnalysisModule> AnalysisModuleRegistry::Create(const std::string &name) const
{
    ModuleFactory factory;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        auto it = m_Factories.find(name);
        if (it == m_Factories.end()) throw std::runtime_error("Module not found: " + name);
        factory = it->second;
    }
    return factory();
}

std::vector<std::string> AnalysisModuleRegistry::ListModules() const
{
    std::vector<std::string> names;
    std::lock_guard<std::mutex> lock(m_Mutex);
    for (const auto &kv : m_Factories)
        names.push_back(kv.first);
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<ModuleMetadata> AnalysisModuleRegistry::ListModuleMetadata() const
{
    std::vector<std::pair<std::string, MetadataProvider>> providers;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        providers.reserve(m_Factories.size());
        for (const auto &[name, _] : m_Factories)
        {
            auto provider = m_MetadataProviders.find(name);
            providers.emplace_back(name, provider == m_MetadataProviders.end() ? MetadataProvider{} : provider->second);
        }
    }
    std::sort(providers.begin(), providers.end(), [](const auto &left, const auto &right) { return left.first < right.first; });

    std::vector<ModuleMetadata> metadata;
    metadata.reserve(providers.size());
    for (const auto &[name, provider] : providers)
    {
        if (provider)
        {
            try
            {
                auto info = provider();
                if (info.Name.empty()) info.Name = name;
                metadata.push_back(std::move(info));
                continue;
            }
            catch (const std::exception &error)
            {
                LOG_WARN("REGISTRY", "Metadata provider failed for " << name << ": " << error.what());
            }
            catch (...)
            {
                LOG_WARN("REGISTRY", "Metadata provider failed for " << name << " with an unknown exception");
            }
        }
        ModuleMetadata info;
        info.Name = name;
        info.Summary = "metadata unavailable";
        metadata.push_back(std::move(info));
    }
    return metadata;
}
