#pragma once
#include <memory>

#define CASCADE_PLUGIN_ABI_VERSION 1

#ifdef __cplusplus
extern "C" {
#endif

int CascadePluginAbiVersion();
void CascadeRegisterPlugin();

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#include "AnalysisModuleRegistry.hh"

#define CASCADE_PLUGIN_EXPORT extern "C"

#define CASCADE_REGISTER_MODULE(T)                                                                                                                             \
    AnalysisModuleRegistry::Get().Register(#T, []() -> std::unique_ptr<IAnalysisModule> { return std::make_unique<T>(); });

#endif
