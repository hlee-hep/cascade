#pragma once
#include <filesystem>
#include <fstream>
#include <string>
#include <yaml-cpp/yaml.h>

class CacheManager
{
  public:
    static inline std::string CacheDir()
    {
        const char *home = std::getenv("HOME");
        if (home && std::string(home).size() > 0)
        {
            return std::string(home) + "/.cache/cascade/snapshot_cache";
        }
        return ".snapshot_cache";
    }

    static inline bool IsHashCached(const std::string &moduleName, const std::string &hash)
    {
        std::string path = CacheDir() + "/" + moduleName + ".yaml";
        std::ifstream in(path);
        if (!in) return false;

        YAML::Node hashes = YAML::Load(in);
        for (const auto &h : hashes)
        {
            if (h.as<std::string>() == hash) return true;
        }
        return false;
    }

    static inline void AddHash(const std::string &moduleName, const std::string &hash)
    {
        std::string path = CacheDir() + "/" + moduleName + ".yaml";
        YAML::Node node;

        std::ifstream in(path);
        if (in) node = YAML::Load(in);

        for (const auto &h : node)
            if (h.as<std::string>() == hash) return;

        node.push_back(hash);

        std::filesystem::create_directories(CacheDir());
        std::ofstream out(path);
        out << node;
    }
};
