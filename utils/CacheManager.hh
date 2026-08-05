#pragma once

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <sstream>
#include <system_error>
#include <vector>
#include <yaml-cpp/yaml.h>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

struct CacheSnapshot
{
    std::string Module;
    std::string Hash;
    std::string Provenance;
    std::string CacheFile;
};

class CacheManager
{
  private:
    class FileLock
    {
      public:
        FileLock(const std::string &path, int operation)
        {
            int flags = O_CREAT | O_RDWR;
#ifdef O_CLOEXEC
            flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
            flags |= O_NOFOLLOW;
#endif
            m_Descriptor = open(path.c_str(), flags, 0600);
            if (m_Descriptor < 0) throw std::system_error(errno, std::generic_category(), "Cannot open cache lock");
            struct stat metadata{};
            if (fstat(m_Descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode))
            {
                const int error = errno;
                close(m_Descriptor);
                m_Descriptor = -1;
                throw std::system_error(error ? error : EINVAL, std::generic_category(), "Cache lock is not a regular file");
            }
            while (flock(m_Descriptor, operation) != 0)
            {
                if (errno == EINTR) continue;
                const int error = errno;
                close(m_Descriptor);
                m_Descriptor = -1;
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
        return (std::filesystem::path(cacheDirectory) / (SafeName(moduleName) + ".yaml")).string();
    }

    static inline YAML::Node EmptyDocument()
    {
        YAML::Node document(YAML::NodeType::Map);
        document["schema_version"] = 1;
        document["snapshots"] = YAML::Node(YAML::NodeType::Sequence);
        return document;
    }

    static inline YAML::Node ReadDocument(std::istream &input, const std::string &path)
    {
        const YAML::Node document = YAML::Load(input);
        if (!document || !document.IsMap() || !document["snapshots"] || !document["snapshots"].IsSequence())
            throw std::runtime_error("Cascade cache has an invalid schema: " + path);
        if (!document["schema_version"] || document["schema_version"].as<int>() != 1)
            throw std::runtime_error("Cascade cache has an unsupported schema version: " + path);
        for (const auto &entry : document["snapshots"])
        {
            if (!entry.IsMap() || !entry["hash"] || !entry["hash"].IsScalar())
                throw std::runtime_error("Cascade cache has an invalid snapshot entry: " + path);
            if (entry["provenance"] && !entry["provenance"].IsScalar())
                throw std::runtime_error("Cascade cache has an invalid provenance path: " + path);
        }
        return document;
    }

    static inline void WriteDocument(const std::string &path, const YAML::Node &document)
    {
        const std::filesystem::path destination(path);
        std::string pattern = (destination.parent_path() / (destination.filename().string() + ".tmp.XXXXXX")).string();
        std::vector<char> temporary(pattern.begin(), pattern.end());
        temporary.push_back('\0');
        const int descriptor = mkstemp(temporary.data());
        if (descriptor < 0) throw std::system_error(errno, std::generic_category(), "Cannot create cache temporary file");
        const std::string payload = YAML::Dump(document);
        bool descriptorOpen = true;
        try
        {
            std::size_t offset = 0;
            while (offset < payload.size())
            {
                const ssize_t written = write(descriptor, payload.data() + offset, payload.size() - offset);
                if (written < 0 && errno == EINTR) continue;
                if (written <= 0) throw std::system_error(errno ? errno : EIO, std::generic_category(), "Cannot write cache file");
                offset += static_cast<std::size_t>(written);
            }
            if (fchmod(descriptor, 0600) != 0 || fsync(descriptor) != 0)
                throw std::system_error(errno, std::generic_category(), "Cannot flush cache file");
            const int closeResult = close(descriptor);
            descriptorOpen = false;
            if (closeResult != 0) throw std::system_error(errno, std::generic_category(), "Cannot close cache file");
            if (rename(temporary.data(), path.c_str()) != 0)
                throw std::system_error(errno, std::generic_category(), "Cannot replace cache file");
            const int directory = open(destination.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (directory >= 0)
            {
                fsync(directory);
                close(directory);
            }
        }
        catch (...)
        {
            if (descriptorOpen) close(descriptor);
            unlink(temporary.data());
            throw;
        }
    }

    static inline YAML::Node ReadDocumentFile(const std::string &path, bool missingAllowed)
    {
        int flags = O_RDONLY;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
        const int descriptor = open(path.c_str(), flags);
        if (descriptor < 0)
        {
            if (missingAllowed && errno == ENOENT) return EmptyDocument();
            throw std::system_error(errno, std::generic_category(), "Cannot open cache file");
        }
        struct stat metadata{};
        if (fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode))
        {
            const int error = errno ? errno : EINVAL;
            close(descriptor);
            throw std::system_error(error, std::generic_category(), "Cache path is not a regular file");
        }
        std::string payload;
        char buffer[8192];
        while (true)
        {
            const ssize_t count = read(descriptor, buffer, sizeof(buffer));
            if (count < 0 && errno == EINTR) continue;
            if (count < 0)
            {
                const int error = errno;
                close(descriptor);
                throw std::system_error(error, std::generic_category(), "Cannot read cache file");
            }
            if (count == 0) break;
            payload.append(buffer, static_cast<std::size_t>(count));
        }
        close(descriptor);
        std::istringstream input(payload);
        return ReadDocument(input, path);
    }

    static inline std::size_t MaxSnapshots()
    {
        const char *configured = std::getenv("CASCADE_CACHE_MAX_SNAPSHOTS");
        if (!configured || !*configured) return 256;
        std::size_t parsed = 0;
        const std::string value(configured);
        if (!value.empty() && value.front() == '-')
            throw std::runtime_error("CASCADE_CACHE_MAX_SNAPSHOTS must be a non-negative integer");
        const unsigned long long limit = std::stoull(value, &parsed);
        if (parsed != value.size()) throw std::runtime_error("CASCADE_CACHE_MAX_SNAPSHOTS must be a non-negative integer");
        return static_cast<std::size_t>(limit);
    }

    static inline std::vector<CacheSnapshot> ReadSnapshots(const std::string &path, const std::string &moduleName)
    {
        if (!std::filesystem::exists(std::filesystem::path(path).parent_path())) return {};
        FileLock lock(path + ".lock", LOCK_SH);
        const YAML::Node document = ReadDocumentFile(path, true);
        std::vector<CacheSnapshot> snapshots;
        for (const auto &entry : document["snapshots"])
            snapshots.push_back({moduleName,
                                 entry["hash"].as<std::string>(),
                                 entry["provenance"] ? entry["provenance"].as<std::string>() : std::string(),
                                 path});
        return snapshots;
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
        return Lookup(moduleName, hash, cacheDirectory).has_value();
    }

    static inline std::optional<CacheSnapshot> Lookup(const std::string &moduleName, const std::string &hash,
                                                      const std::string &cacheDirectory)
    {
        for (const auto &snapshot : ReadSnapshots(CachePath(moduleName, cacheDirectory), moduleName))
            if (snapshot.Hash == hash) return snapshot;
        return std::nullopt;
    }

    static inline std::string FindProvenance(const std::string &moduleName, const std::string &hash,
                                             const std::string &cacheDirectory)
    {
        const auto snapshot = Lookup(moduleName, hash, cacheDirectory);
        return snapshot ? snapshot->Provenance : std::string();
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
        YAML::Node document = ReadDocumentFile(path, true);
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
        const std::size_t limit = MaxSnapshots();
        if (limit > 0 && document["snapshots"].size() > limit)
        {
            YAML::Node bounded = EmptyDocument();
            const std::size_t first = document["snapshots"].size() - limit;
            for (std::size_t index = first; index < document["snapshots"].size(); ++index)
                bounded["snapshots"].push_back(document["snapshots"][index]);
            document = std::move(bounded);
        }
        WriteDocument(path, document);
    }

    static inline void RemoveHash(const std::string &moduleName, const std::string &hash, const std::string &cacheDirectory)
    {
        const std::string path = CachePath(moduleName, cacheDirectory);
        std::filesystem::create_directories(cacheDirectory);
        FileLock lock(path + ".lock", LOCK_EX);
        const YAML::Node existing = ReadDocumentFile(path, true);
        YAML::Node document = EmptyDocument();
        for (const auto &entry : existing["snapshots"])
            if (entry["hash"].as<std::string>() != hash) document["snapshots"].push_back(entry);
        WriteDocument(path, document);
    }

    static inline std::vector<CacheSnapshot> ListSnapshots(const std::string &cacheDirectory,
                                                           const std::string &moduleName = "")
    {
        std::vector<CacheSnapshot> snapshots;
        if (!moduleName.empty()) return ReadSnapshots(CachePath(moduleName, cacheDirectory), moduleName);
        if (!std::filesystem::is_directory(cacheDirectory)) return snapshots;
        std::vector<std::filesystem::path> paths;
        for (const auto &entry : std::filesystem::directory_iterator(cacheDirectory))
            if (entry.symlink_status().type() == std::filesystem::file_type::regular && entry.path().extension() == ".yaml")
                paths.push_back(entry.path());
        std::sort(paths.begin(), paths.end());
        for (const auto &path : paths)
        {
            auto entries = ReadSnapshots(path.string(), path.stem().string());
            snapshots.insert(snapshots.end(), entries.begin(), entries.end());
        }
        return snapshots;
    }

    static inline std::vector<CacheSnapshot> Prune(const std::string &cacheDirectory, const std::string &moduleName,
                                                   bool removeAll, bool dryRun = false)
    {
        std::vector<CacheSnapshot> removed;
        std::vector<std::filesystem::path> paths;
        if (!moduleName.empty())
            paths.emplace_back(CachePath(moduleName, cacheDirectory));
        else if (std::filesystem::is_directory(cacheDirectory))
            for (const auto &entry : std::filesystem::directory_iterator(cacheDirectory))
                if (entry.symlink_status().type() == std::filesystem::file_type::regular && entry.path().extension() == ".yaml")
                    paths.push_back(entry.path());
        std::sort(paths.begin(), paths.end());
        for (const auto &path : paths)
        {
            if (!std::filesystem::is_regular_file(path)) continue;
            FileLock lock(path.string() + ".lock", LOCK_EX);
            const YAML::Node existing = ReadDocumentFile(path.string(), false);
            YAML::Node document = EmptyDocument();
            const std::string recordedModule = moduleName.empty() ? path.stem().string() : moduleName;
            for (const auto &entry : existing["snapshots"])
            {
                const std::string provenance = entry["provenance"] ? entry["provenance"].as<std::string>() : std::string();
                const bool stale = !provenance.empty() && !std::filesystem::is_regular_file(provenance);
                if (removeAll || stale)
                    removed.push_back({recordedModule, entry["hash"].as<std::string>(), provenance, path.string()});
                else
                    document["snapshots"].push_back(entry);
            }
            if (!dryRun && document["snapshots"].size() != existing["snapshots"].size()) WriteDocument(path.string(), document);
        }
        return removed;
    }
};
