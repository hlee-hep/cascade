#include "AnalysisModuleRegistry.hh"
#include "IAnalysisModule.hh"

AnalysisModuleRegistry &AnalysisModuleRegistry::Get()
{
    static AnalysisModuleRegistry instance;
    return instance;
}

void AnalysisModuleRegistry::Register(const std::string &name, ModuleFactory factory) { m_Factories[name] = std::move(factory); }

void AnalysisModuleRegistry::Register(const std::string &name, ModuleFactory factory, MetadataProvider metadata)
{
    m_Factories[name] = std::move(factory);
    m_MetadataProviders[name] = std::move(metadata);
}

std::unique_ptr<IAnalysisModule> AnalysisModuleRegistry::Create(const std::string &name) const
{
    auto it = m_Factories.find(name);
    if (it == m_Factories.end())
    {
        throw std::runtime_error("Module not found: " + name);
    }
    return it->second();
}

std::vector<std::string> AnalysisModuleRegistry::ListModules() const
{
    std::vector<std::string> names;
    for (const auto &kv : m_Factories)
        names.push_back(kv.first);
    return names;
}

std::vector<ModuleMetadata> AnalysisModuleRegistry::ListModuleMetadata() const
{
    std::vector<ModuleMetadata> metadata;
    metadata.reserve(m_Factories.size());
    for (const auto &kv : m_Factories)
    {
        auto it = m_MetadataProviders.find(kv.first);
        if (it != m_MetadataProviders.end())
        {
            try
            {
                auto info = it->second();
                if (info.Name.empty()) info.Name = kv.first;
                metadata.push_back(std::move(info));
                continue;
            }
            catch (...)
            {
            }
        }
        ModuleMetadata info;
        info.Name = kv.first;
        info.Summary = "metadata unavailable";
        metadata.push_back(std::move(info));
    }
    return metadata;
}
