#pragma once

#include <cstddef>
#include <string>

struct RuntimeOptions
{
    std::string InputHash = "metadata";
    std::string OutputHash = "full";
    std::size_t DagWorkers = 0;
    double IsolatedTimeoutSeconds = 0.0;
    std::size_t ProgressIntervalMilliseconds = 200;
    std::size_t ArtifactHashCacheEntries = 1024;
};

RuntimeOptions GetRuntimeOptions();
void SetRuntimeOptions(const RuntimeOptions &options);

class RuntimeOptionsScope
{
  public:
    explicit RuntimeOptionsScope(const RuntimeOptions &options);
    ~RuntimeOptionsScope();

    RuntimeOptionsScope(const RuntimeOptionsScope &) = delete;
    RuntimeOptionsScope &operator=(const RuntimeOptionsScope &) = delete;

  private:
    RuntimeOptions m_Options;
    const RuntimeOptions *m_Previous = nullptr;
};
