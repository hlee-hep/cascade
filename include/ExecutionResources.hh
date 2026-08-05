#pragma once

#include <mutex>

inline std::recursive_mutex &CascadeRootExecutionMutex()
{
    static std::recursive_mutex mutex;
    return mutex;
}
