#pragma once
#include "AnalysisModuleRegistry.hh"

#if defined(CASCADE_PLUGIN_NO_AUTO_REGISTER)
#define REGISTER_MODULE(T)
#define REGISTER_MODULE_WITH_METADATA(T, META_EXPR)
#else
#define REGISTER_MODULE(T)                                                                                                                                     \
    namespace                                                                                                                                                  \
    {                                                                                                                                                          \
    struct T##RegistryEntry                                                                                                                                    \
    {                                                                                                                                                          \
        T##RegistryEntry()                                                                                                                                     \
        {                                                                                                                                                      \
            RegisterAnalysisModuleType<T>(#T);                                                                                                                 \
        }                                                                                                                                                      \
    };                                                                                                                                                         \
    static T##RegistryEntry g_##T##RegistryEntry;                                                                                                              \
    }
#define REGISTER_MODULE_WITH_METADATA(T, META_EXPR)                                                                                                            \
    namespace                                                                                                                                                  \
    {                                                                                                                                                          \
    struct T##RegistryEntry                                                                                                                                    \
    {                                                                                                                                                          \
        T##RegistryEntry()                                                                                                                                     \
        {                                                                                                                                                      \
            RegisterAnalysisModuleType<T>(#T, []() -> ModuleMetadata                                                                                           \
                                          {                                                                                                                     \
                                              ModuleMetadata info = META_EXPR;                                                                                  \
                                              if (info.Name.empty()) info.Name = #T;                                                                            \
                                              return info;                                                                                                      \
                                          });                                                                                                                   \
        }                                                                                                                                                      \
    };                                                                                                                                                         \
    static T##RegistryEntry g_##T##RegistryEntry;                                                                                                              \
    }
#endif
