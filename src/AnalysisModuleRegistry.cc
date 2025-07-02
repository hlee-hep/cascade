#include "AnalysisModuleRegistry.hh"
#include "IAnalysisModule.hh"

AnalysisModuleRegistry &AnalysisModuleRegistry::Get()
{
    static AnalysisModuleRegistry instance;
    return instance;
}

void AnalysisModuleRegistry::Register(const std::string &name,
                                      ModuleFactory factory)
{
    factories_[name] = std::move(factory);
}

std::unique_ptr<IAnalysisModule>
AnalysisModuleRegistry::Create(const std::string &name) const
{
    auto it = factories_.find(name);
    if (it == factories_.end())
    {
        throw std::runtime_error("Module not found: " + name);
    }
    return it->second();
}

std::vector<std::string> AnalysisModuleRegistry::ListModules() const
{
    std::vector<std::string> names;
    for (const auto &kv : factories_)
        names.push_back(kv.first);
    return names;
}
