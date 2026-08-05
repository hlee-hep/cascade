#pragma once
#include <RVersion.h>
#include <cstdint>
#include <memory>

#define CASCADE_PLUGIN_ABI_VERSION 2

#define CASCADE_STRINGIFY_DETAIL(value) #value
#define CASCADE_STRINGIFY(value) CASCADE_STRINGIFY_DETAIL(value)

#if defined(__clang__)
#define CASCADE_COMPILER_STR                                                                                                                                    \
    "clang-" CASCADE_STRINGIFY(__clang_major__) "." CASCADE_STRINGIFY(__clang_minor__) "." CASCADE_STRINGIFY(__clang_patchlevel__)
#elif defined(__GNUC__)
#define CASCADE_COMPILER_STR                                                                                                                                    \
    "gcc-" CASCADE_STRINGIFY(__GNUC__) "." CASCADE_STRINGIFY(__GNUC_MINOR__) "." CASCADE_STRINGIFY(__GNUC_PATCHLEVEL__)
#else
#define CASCADE_COMPILER_STR "unknown-compiler"
#endif

#if defined(_LIBCPP_VERSION)
#define CASCADE_STDLIB_STR "libc++-" CASCADE_STRINGIFY(_LIBCPP_VERSION)
#elif defined(__GLIBCXX__)
#if defined(_GLIBCXX_RELEASE)
#define CASCADE_STDLIB_STR "libstdc++-" CASCADE_STRINGIFY(_GLIBCXX_RELEASE) "-" CASCADE_STRINGIFY(__GLIBCXX__)
#else
#define CASCADE_STDLIB_STR "libstdc++-" CASCADE_STRINGIFY(__GLIBCXX__)
#endif
#else
#define CASCADE_STDLIB_STR "unknown-stdlib"
#endif

#if defined(_GLIBCXX_USE_CXX11_ABI)
#define CASCADE_STDLIB_ABI_STR CASCADE_STRINGIFY(_GLIBCXX_USE_CXX11_ABI)
#else
#define CASCADE_STDLIB_ABI_STR "unknown"
#endif

#if INTPTR_MAX == INT64_MAX
#define CASCADE_POINTER_WIDTH_STR "64"
#elif INTPTR_MAX == INT32_MAX
#define CASCADE_POINTER_WIDTH_STR "32"
#else
#define CASCADE_POINTER_WIDTH_STR "unknown"
#endif

#if defined(_GLIBCXX_DEBUG)
#define CASCADE_GLIBCXX_DEBUG_STR "1"
#else
#define CASCADE_GLIBCXX_DEBUG_STR "0"
#endif

#if defined(NDEBUG)
#define CASCADE_BUILD_MODE_STR "release"
#else
#define CASCADE_BUILD_MODE_STR "debug"
#endif

#define CASCADE_ABI_TAG                                                                                                                                            \
    "abi=2;cxx=" CASCADE_STRINGIFY(__cplusplus) ";compiler=" CASCADE_COMPILER_STR ";stdlib=" CASCADE_STDLIB_STR                           \
    ";cxx11abi=" CASCADE_STDLIB_ABI_STR ";root=" ROOT_RELEASE ";ptr=" CASCADE_POINTER_WIDTH_STR                                      \
    ";build=" CASCADE_BUILD_MODE_STR ";glibcxx_debug=" CASCADE_GLIBCXX_DEBUG_STR

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

#define CASCADE_PLUGIN_EXPORT extern "C"

#define CASCADE_REGISTER_MODULE(T)                                                                                                                             \
    RegisterAnalysisModuleType<T>(#T);

#define CASCADE_REGISTER_MODULE_WITH_METADATA(T, META_EXPR)                                                                                                    \
    RegisterAnalysisModuleType<T>(#T, []() -> ModuleMetadata                                                                                                   \
        {                                                                                                                                                       \
            ModuleMetadata info = META_EXPR;                                                                                                                    \
            if (info.Name.empty()) info.Name = #T;                                                                                                              \
            return info;                                                                                                                                        \
        });

#define CASCADE_PLUGIN_EXPORT_ABI                                                                                                                               \
    CASCADE_PLUGIN_EXPORT int CascadePluginAbiVersion() { return CASCADE_PLUGIN_ABI_VERSION; }                                                                  \
    CASCADE_PLUGIN_EXPORT const char *CascadePluginAbiTag() { return CASCADE_ABI_TAG; }

#endif
