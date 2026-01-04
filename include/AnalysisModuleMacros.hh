#pragma once
#include "AnalysisModuleRegistry.hh"

#define REGISTER_MODULE(T)                                                                                                                                     \
    namespace                                                                                                                                                  \
    {                                                                                                                                                          \
    struct T##RegistryEntry                                                                                                                                    \
    {                                                                                                                                                          \
        T##RegistryEntry()                                                                                                                                     \
        {                                                                                                                                                      \
            AnalysisModuleRegistry::Get().Register(#T, []() -> std::unique_ptr<IAnalysisModule> { return std::make_unique<T>(); });                            \
        }                                                                                                                                                      \
    };                                                                                                                                                         \
    static T##RegistryEntry g_##T##RegistryEntry;                                                                                                              \
    }
