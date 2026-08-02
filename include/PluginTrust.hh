#pragma once

#include <string>

enum class PluginTrustPolicy
{
    Verified,
    RequireSigned
};

enum class PluginTrustStatus
{
    Verified,
    Signed
};

inline const char *ToString(PluginTrustPolicy policy)
{
    switch (policy)
    {
    case PluginTrustPolicy::Verified:
        return "Verified";
    case PluginTrustPolicy::RequireSigned:
        return "RequireSigned";
    }
    return "Unknown";
}

inline const char *ToString(PluginTrustStatus status)
{
    switch (status)
    {
    case PluginTrustStatus::Verified:
        return "Verified";
    case PluginTrustStatus::Signed:
        return "Signed";
    }
    return "Unknown";
}

struct PluginOrigin
{
    std::string Package;
    std::string ManifestPath;
    std::string ManifestSha256;
    std::string ArtifactSha256;
    std::string SignerFingerprint;
    PluginTrustStatus Trust = PluginTrustStatus::Verified;
};
