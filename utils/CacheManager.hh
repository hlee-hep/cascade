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

        YAML::Node hashes = YAML::Load(input);
        if (hashes && !hashes.IsSequence()) throw std::runtime_error("Cascade cache is not a YAML sequence: " + path);
        for (const auto &entry : hashes)
            if (entry.as<std::string>() == hash) return true;
        return false;
    }

    static inline void AddHash(const std::string &moduleName, const std::string &hash)
    {
        AddHash(moduleName, hash, CacheDir());
    }

    static inline void AddHash(const std::string &moduleName, const std::string &hash, const std::string &cacheDirectory)
    {
        const std::string path = CachePath(moduleName, cacheDirectory);
        std::filesystem::create_directories(cacheDirectory);
        FileLock lock(path + ".lock", LOCK_EX);

        YAML::Node hashes(YAML::NodeType::Sequence);
        std::ifstream input(path);
        if (input)
        {
            hashes = YAML::Load(input);
            if (hashes && !hashes.IsSequence()) throw std::runtime_error("Cascade cache is not a YAML sequence: " + path);
        }
        for (const auto &entry : hashes)
            if (entry.as<std::string>() == hash) return;
        hashes.push_back(hash);

        const std::string temporary = path + ".tmp." + std::to_string(getpid());
        {
            std::ofstream output(temporary, std::ios::trunc);
            if (!output) throw std::runtime_error("Cannot write cache file: " + temporary);
            output << hashes;
            if (!output) throw std::runtime_error("Failed while writing cache file: " + temporary);
        }
        std::filesystem::rename(temporary, path);
    }

    static inline void RemoveHash(const std::string &moduleName, const std::string &hash, const std::string &cacheDirectory)
    {
        const std::string path = CachePath(moduleName, cacheDirectory);
        std::filesystem::create_directories(cacheDirectory);
        FileLock lock(path + ".lock", LOCK_EX);

        YAML::Node hashes(YAML::NodeType::Sequence);
        std::ifstream input(path);
        if (!input) return;
        YAML::Node existing = YAML::Load(input);
        if (existing && !existing.IsSequence()) throw std::runtime_error("Cascade cache is not a YAML sequence: " + path);
        for (const auto &entry : existing)
            if (entry.as<std::string>() != hash) hashes.push_back(entry.as<std::string>());

        const std::string temporary = path + ".tmp." + std::to_string(getpid());
        {
            std::ofstream output(temporary, std::ios::trunc);
            if (!output) throw std::runtime_error("Cannot write cache file: " + temporary);
            output << hashes;
            if (!output) throw std::runtime_error("Failed while writing cache file: " + temporary);
        }
        std::filesystem::rename(temporary, path);
    }
};
