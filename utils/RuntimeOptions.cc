#include "RuntimeOptions.hh"

#include <cmath>
#include <mutex>
#include <stdexcept>

namespace
{
void Validate(const RuntimeOptions &options)
{
    if (options.InputHash != "metadata" && options.InputHash != "auto" && options.InputHash != "full")
        throw std::invalid_argument("input_hash must be metadata, auto, or full");
    if (options.OutputHash != "full" && options.OutputHash != "metadata" && options.OutputHash != "none")
        throw std::invalid_argument("output_hash must be full, metadata, or none");
    if (!std::isfinite(options.IsolatedTimeoutSeconds) || options.IsolatedTimeoutSeconds < 0.0)
        throw std::invalid_argument("isolated_timeout_seconds must be non-negative");
}

std::mutex g_RuntimeOptionsMutex;
RuntimeOptions g_RuntimeOptions;
thread_local const RuntimeOptions *g_ActiveRuntimeOptions = nullptr;
} // namespace

RuntimeOptions GetRuntimeOptions()
{
    if (g_ActiveRuntimeOptions) return *g_ActiveRuntimeOptions;
    std::lock_guard<std::mutex> lock(g_RuntimeOptionsMutex);
    return g_RuntimeOptions;
}

RuntimeOptionsScope::RuntimeOptionsScope(const RuntimeOptions &options)
    : m_Options(options), m_Previous(g_ActiveRuntimeOptions)
{
    Validate(m_Options);
    g_ActiveRuntimeOptions = &m_Options;
}

RuntimeOptionsScope::~RuntimeOptionsScope() { g_ActiveRuntimeOptions = m_Previous; }

void SetRuntimeOptions(const RuntimeOptions &options)
{
    Validate(options);
    std::lock_guard<std::mutex> lock(g_RuntimeOptionsMutex);
    g_RuntimeOptions = options;
}
