#include "AnalysisModuleRegistry.hh"
#include "IAnalysisModule.hh"

AnalysisModuleRegistry &AnalysisModuleRegistry::Get()
{
    static AnalysisModuleRegistry instance;
    return instance;
}

void AnalysisModuleRegistry::Register(const std::string &name, ModuleFactory factory) { m_Factories[name] = std::move(factory); }

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
