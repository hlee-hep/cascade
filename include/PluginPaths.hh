#pragma once

#include <string>
#include <vector>

struct PluginLayout
{
    std::string Prefix;
    std::string Cpp;
    std::string Python;
    std::string Include;
    std::string TrustStore;
    std::string Source;
};

struct PluginPrefix
{
    std::string Path;
    bool Enabled = true;
};

struct PluginConfig
{
    int Schema = 1;
    std::vector<PluginPrefix> Prefixes;
};

class PluginPaths
{
  public:
    static std::string ConfigPath();
    static std::string RuntimePrefix();
    static std::string CanonicalPrefix(const std::string &path);
    static std::vector<std::string> Unique(const std::vector<std::string> &paths);
    static PluginConfig LoadConfig(const std::string &path = "");
    static std::vector<std::string> ConfiguredPrefixes(const std::string &path = "");
    static bool AddPrefix(const std::string &prefix, const std::string &path = "");
    static bool RemovePrefix(const std::string &prefix, const std::string &path = "");
    static PluginLayout Layout(const std::string &prefix);
    static std::vector<PluginLayout> RuntimeLayouts();
    static std::vector<std::string> Roots(const std::string &language);
    static std::string TrustStoreForRoot(const std::string &pluginRoot);
};
