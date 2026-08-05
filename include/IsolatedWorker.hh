#pragma once

#include "ModuleRun.hh"

#include <cstdint>
#include <string>

inline constexpr std::uint32_t kCascadeWorkerResultMagic = 0x43534344;
inline constexpr std::uint32_t kCascadeWorkerMaxMessageSize = 4096;

struct IsolatedRunHeader
{
    std::uint32_t Magic = kCascadeWorkerResultMagic;
    std::int32_t Status = static_cast<std::int32_t>(ModuleStatus::Failed);
    std::int32_t Phase = static_cast<std::int32_t>(ModulePhase::Execute);
    std::uint32_t MessageSize = 0;
};
