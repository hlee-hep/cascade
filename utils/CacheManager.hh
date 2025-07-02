#pragma once
#include <filesystem>
#include <fstream>
#include <string>
#include <yaml-cpp/yaml.h>

class CacheManager
{
  public:
    static inline bool IsHashCached(const std::string &moduleName,
                                    const std::string &hash)
    {
        std::string path = ".snapshot_cache/" + moduleName + ".yaml";
        std::ifstream in(path);
        if (!in)
            return false;

        YAML::Node hashes = YAML::Load(in);
        for (const auto &h : hashes)
        {
            if (h.as<std::string>() == hash)
                return true;
        }
        return false;
    }

    static inline void AddHash(const std::string &moduleName,
                               const std::string &hash)
    {
        std::string path = ".snapshot_cache/" + moduleName + ".yaml";
        YAML::Node node;

        std::ifstream in(path);
        if (in)
            node = YAML::Load(in);

        for (const auto &h : node)
            if (h.as<std::string>() == hash)
                return;

        node.push_back(hash);

        std::filesystem::create_directories(".snapshot_cache");
        std::ofstream out(path);
        out << node;
    }
};