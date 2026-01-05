#pragma once
#include <memory>

#define CASCADE_PLUGIN_ABI_VERSION 1

#if defined(__clang__)
#define CASCADE_COMPILER_STR "clang-" __clang_version__
#elif defined(__GNUC__)
#define CASCADE_COMPILER_STR "gcc-" __VERSION__
#else
#define CASCADE_COMPILER_STR "unknown-compiler"
#endif

#if defined(_LIBCPP_VERSION)
#define CASCADE_STDLIB_STR "libc++"
#elif defined(__GLIBCXX__)
#define CASCADE_STDLIB_STR "libstdc++"
#else
#define CASCADE_STDLIB_STR "unknown-stdlib"
#endif

#define CASCADE_ABI_TAG "abi=" CASCADE_VERSION_STRING ";cxx=17;compiler=" CASCADE_COMPILER_STR ";stdlib=" CASCADE_STDLIB_STR

#ifdef __cplusplus
extern "C" {
#endif

int CascadePluginAbiVersion();
const char *CascadePluginAbiTag();
void CascadeRegisterPlugin();

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#include "AnalysisModuleRegistry.hh"
#include "Version.hh"

#define CASCADE_PLUGIN_EXPORT extern "C"

#define CASCADE_REGISTER_MODULE(T)                                                                                                                             \
    AnalysisModuleRegistry::Get().Register(#T, []() -> std::unique_ptr<IAnalysisModule> { return std::make_unique<T>(); });

#define CASCADE_PLUGIN_EXPORT_ABI                                                                                                                               \
    CASCADE_PLUGIN_EXPORT int CascadePluginAbiVersion() { return CASCADE_PLUGIN_ABI_VERSION; }                                                                  \
    CASCADE_PLUGIN_EXPORT const char *CascadePluginAbiTag() { return CASCADE_ABI_TAG; }

#endif
