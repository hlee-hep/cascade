#pragma once

#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <yaml-cpp/yaml.h>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

class CacheManager
{
  private:
    class FileLock
    {
      public:
        FileLock(const std::string &path, int operation)
        {
            m_Descriptor = open(path.c_str(), O_CREAT | O_RDWR, 0600);
            if (m_Descriptor < 0) throw std::system_error(errno, std::generic_category(), "Cannot open cache lock");
            if (flock(m_Descriptor, operation) != 0)
            {
                const int error = errno;
                close(m_Descriptor);
                throw std::system_error(error, std::generic_category(), "Cannot lock cache");
            }
        }
        FileLock(const FileLock &) = delete;
        FileLock &operator=(const FileLock &) = delete;
        ~FileLock()
        {
            if (m_Descriptor >= 0)
            {
                flock(m_Descriptor, LOCK_UN);
                close(m_Descriptor);
            }
        }

      private:
        int m_Descriptor = -1;
    };

    static inline std::string SafeName(std::string value)
    {
        for (char &character : value)
            if (!std::isalnum(static_cast<unsigned char>(character)) && character != '-' && character != '_') character = '_';
        return value.empty() ? "unnamed" : value;
    }

    static inline std::string CachePath(const std::string &moduleName, const std::string &cacheDirectory)
    {
        return cacheDirectory + "/" + SafeName(moduleName) + ".yaml";
    }

    static inline YAML::Node ReadDocument(std::istream &input, const std::string &path)
    {
        YAML::Node existing = YAML::Load(input);
        YAML::Node document(YAML::NodeType::Map);
        document["schema_version"] = 1;
        document["snapshots"] = YAML::Node(YAML::NodeType::Sequence);
        if (!existing) return document;
        if (existing.IsSequence())
        {
            for (const auto &hash : existing)
            {
                YAML::Node entry(YAML::NodeType::Map);
                entry["hash"] = hash.as<std::string>();
                entry["provenance"] = "";
                document["snapshots"].push_back(entry);
            }
            return document;
        }
        if (!existing.IsMap() || !existing["snapshots"] || !existing["snapshots"].IsSequence())
            throw std::runtime_error("Cascade cache has an invalid schema: " + path);
        if (!existing["schema_version"] || existing["schema_version"].as<int>() != 1)
            throw std::runtime_error("Cascade cache has an unsupported schema version: " + path);
        return existing;
    }

    static inline void WriteDocument(const std::string &path, const YAML::Node &document)
    {
        const std::string temporary = path + ".tmp." + std::to_string(getpid());
        {
            std::ofstream output(temporary, std::ios::trunc);
            if (!output) throw std::runtime_error("Cannot write cache file: " + temporary);
            output << document;
            if (!output) throw std::runtime_error("Failed while writing cache file: " + temporary);
        }
        std::filesystem::rename(temporary, path);
    }

  public:
    static inline std::string CacheDir()
    {
        if (const char *configured = std::getenv("CASCADE_CACHE_DIR"); configured && *configured) return configured;
        const char *home = std::getenv("HOME");
        if (home && *home) return std::string(home) + "/.cache/cascade/snapshot_cache";
        return ".snapshot_cache";
    }

    static inline bool IsHashCached(const std::string &moduleName, const std::string &hash)
    {
        return IsHashCached(moduleName, hash, CacheDir());
    }

    static inline bool IsHashCached(const std::string &moduleName, const std::string &hash, const std::string &cacheDirectory)
    {
        const std::string path = CachePath(moduleName, cacheDirectory);
        std::filesystem::create_directories(cacheDirectory);
        FileLock lock(path + ".lock", LOCK_SH);
        std::ifstream input(path);
        if (!input) return false;

        const YAML::Node document = ReadDocument(input, path);
        for (const auto &entry : document["snapshots"])
            if (entry["hash"].as<std::string>() == hash) return true;
        return false;
    }

    static inline std::string FindProvenance(const std::string &moduleName, const std::string &hash,
                                             const std::string &cacheDirectory)
    {
        const std::string path = CachePath(moduleName, cacheDirectory);
        std::filesystem::create_directories(cacheDirectory);
        FileLock lock(path + ".lock", LOCK_SH);
        std::ifstream input(path);
        if (!input) return {};
        const YAML::Node document = ReadDocument(input, path);
        for (const auto &entry : document["snapshots"])
            if (entry["hash"].as<std::string>() == hash)
                return entry["provenance"] ? entry["provenance"].as<std::string>() : std::string();
        return {};
    }

    static inline void AddHash(const std::string &moduleName, const std::string &hash)
    {
        AddHash(moduleName, hash, CacheDir(), "");
    }

    static inline void AddHash(const std::string &moduleName, const std::string &hash, const std::string &cacheDirectory,
                               const std::string &provenancePath = "")
    {
        const std::string path = CachePath(moduleName, cacheDirectory);
        std::filesystem::create_directories(cacheDirectory);
        FileLock lock(path + ".lock", LOCK_EX);

        YAML::Node document(YAML::NodeType::Map);
        document["schema_version"] = 1;
        document["snapshots"] = YAML::Node(YAML::NodeType::Sequence);
        std::ifstream input(path);
        if (input) document = ReadDocument(input, path);
        for (auto entry : document["snapshots"])
        {
            if (entry["hash"].as<std::string>() != hash) continue;
            if (!provenancePath.empty())
            {
                entry["provenance"] = provenancePath;
                WriteDocument(path, document);
            }
            return;
        }
        YAML::Node entry(YAML::NodeType::Map);
        entry["hash"] = hash;
        entry["provenance"] = provenancePath;
        document["snapshots"].push_back(entry);
        WriteDocument(path, document);
    }

    static inline void RemoveHash(const std::string &moduleName, const std::string &hash, const std::string &cacheDirectory)
    {
        const std::string path = CachePath(moduleName, cacheDirectory);
        std::filesystem::create_directories(cacheDirectory);
        FileLock lock(path + ".lock", LOCK_EX);

        std::ifstream input(path);
        if (!input) return;
        YAML::Node existing = ReadDocument(input, path);
        YAML::Node document(YAML::NodeType::Map);
        document["schema_version"] = 1;
        document["snapshots"] = YAML::Node(YAML::NodeType::Sequence);
        for (const auto &entry : existing["snapshots"])
        {
            if (entry["hash"].as<std::string>() != hash) document["snapshots"].push_back(entry);
        }
        WriteDocument(path, document);
    }
};
