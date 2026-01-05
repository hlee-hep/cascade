#pragma once
#include "AnalysisModuleRegistry.hh"
#include "Version.hh"

#if defined(CASCADE_PLUGIN_NO_AUTO_REGISTER)
#define REGISTER_MODULE(T)
#else
#define REGISTER_MODULE(T)                                                                                                                                     \
    namespace                                                                                                                                                  \
    {                                                                                                                                                          \
    struct T##RegistryEntry                                                                                                                                    \
    {                                                                                                                                                          \
        T##RegistryEntry()                                                                                                                                     \
        {                                                                                                                                                      \
            AnalysisModuleRegistry::Get().Register(#T,                                                                                                          \
                                                   []() -> std::unique_ptr<IAnalysisModule> { return std::make_unique<T>(); },                                 \
                                                   []() -> ModuleMetadata                                                                                      \
                                                   {                                                                                                            \
                                                       ModuleMetadata info;                                                                                     \
                                                       info.Name = #T;                                                                                          \
                                                       info.Version = CascadeVersionString();                                                                   \
                                                       return info;                                                                                            \
                                                   });                                                                                                          \
        }                                                                                                                                                      \
    };                                                                                                                                                         \
    static T##RegistryEntry g_##T##RegistryEntry;                                                                                                              \
    }
#endif

#define REGISTER_MODULE_WITH_METADATA(T, META_EXPR)                                                                                                            \
    namespace                                                                                                                                                  \
    {                                                                                                                                                          \
    struct T##RegistryEntry                                                                                                                                    \
    {                                                                                                                                                          \
        T##RegistryEntry()                                                                                                                                     \
        {                                                                                                                                                      \
            AnalysisModuleRegistry::Get().Register(#T,                                                                                                          \
                                                   []() -> std::unique_ptr<IAnalysisModule> { return std::make_unique<T>(); },                                 \
                                                   []() -> ModuleMetadata                                                                                      \
                                                   {                                                                                                            \
                                                       ModuleMetadata info = META_EXPR;                                                                        \
                                                       if (info.Name.empty()) info.Name = #T;                                                                  \
                                                       return info;                                                                                            \
                                                   });                                                                                                          \
        }                                                                                                                                                      \
    };                                                                                                                                                         \
    static T##RegistryEntry g_##T##RegistryEntry;                                                                                                              \
    }
