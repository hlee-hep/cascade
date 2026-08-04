#include "PluginPaths.hh"

#include <dlfcn.h>
#include <algorithm>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <fcntl.h>
#include <set>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
namespace fs = std::filesystem;

fs::path Expanded(const std::string &value)
{
    if (value == "~" || value.rfind("~/", 0) == 0)
    {
        const char *home = std::getenv("HOME");
        if (home && *home) return fs::path(home) / value.substr(value == "~" ? 1 : 2);
    }
    return value;
}

std::string Absolute(const fs::path &path)
{
    std::error_code error;
    fs::path result = fs::weakly_canonical(fs::absolute(path), error);
    if (error) result = fs::absolute(path).lexically_normal();
    return result.string();
}

std::string LexicalAbsolute(const fs::path &path) { return fs::absolute(path).lexically_normal().string(); }

void EnsureRealDirectory(const fs::path &directory)
{
    const fs::path absolute = fs::absolute(directory).lexically_normal();
    fs::path current = absolute.root_path();
    for (const auto &component : absolute.relative_path())
    {
        current /= component;
        std::error_code error;
        const auto status = fs::symlink_status(current, error);
        if (error == std::errc::no_such_file_or_directory || status.type() == fs::file_type::not_found)
        {
            error.clear();
            if (!fs::create_directory(current, error) && error)
                throw std::runtime_error("Cannot create Cascade config directory: " + current.string());
            fs::permissions(current, fs::perms::owner_all, fs::perm_options::replace, error);
            continue;
        }
        if (error) throw std::runtime_error("Cannot inspect Cascade config parent: " + current.string());
        if (fs::is_symlink(status))
            throw std::runtime_error("Cascade config parent must not be a symbolic link: " + current.string());
        if (!fs::is_directory(status))
            throw std::runtime_error("Cascade config parent is not a directory: " + current.string());
    }
}

class ConfigLock
{
  public:
    explicit ConfigLock(const fs::path &configPath)
    {
        EnsureRealDirectory(configPath.parent_path());
        const std::string lockPath = configPath.string() + ".lock";
        m_Descriptor = open(lockPath.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (m_Descriptor < 0) throw std::runtime_error("Cannot open Cascade config lock: " + lockPath);
        struct stat metadata{};
        if (fstat(m_Descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode))
        {
            close(m_Descriptor);
            throw std::runtime_error("Cascade config lock must be a regular file: " + lockPath);
        }
        if (flock(m_Descriptor, LOCK_EX) != 0)
        {
            close(m_Descriptor);
            throw std::runtime_error("Cannot lock Cascade config: " + lockPath);
        }
    }

    ~ConfigLock()
    {
        if (m_Descriptor >= 0)
        {
            flock(m_Descriptor, LOCK_UN);
            close(m_Descriptor);
        }
    }

    ConfigLock(const ConfigLock &) = delete;
    ConfigLock &operator=(const ConfigLock &) = delete;

  private:
    int m_Descriptor = -1;
};

nlohmann::json ConfigJson(const PluginConfig &config)
{
    nlohmann::json entries = nlohmann::json::array();
    for (const auto &entry : config.Prefixes) entries.push_back({{"enabled", entry.Enabled}, {"path", entry.Path}});
    return {{"plugin_prefixes", std::move(entries)}, {"schema", config.Schema}};
}

void AtomicWriteConfig(const PluginConfig &config, const fs::path &configPath)
{
    EnsureRealDirectory(configPath.parent_path());
    std::string temporary = (configPath.parent_path() / "config.XXXXXX").string();
    std::vector<char> name(temporary.begin(), temporary.end());
    name.push_back('\0');
    const int descriptor = mkstemp(name.data());
    if (descriptor < 0) throw std::runtime_error("Cannot create temporary Cascade config");
    const std::string payload = ConfigJson(config).dump(2) + '\n';
    bool success = false;
    try
    {
        std::size_t offset = 0;
        while (offset < payload.size())
        {
            const ssize_t written = write(descriptor, payload.data() + offset, payload.size() - offset);
            if (written < 0)
            {
                if (errno == EINTR) continue;
                throw std::runtime_error("Cannot write temporary Cascade config");
            }
            offset += static_cast<std::size_t>(written);
        }
        if (fsync(descriptor) != 0 || fchmod(descriptor, 0600) != 0)
            throw std::runtime_error("Cannot flush temporary Cascade config");
        if (close(descriptor) != 0) throw std::runtime_error("Cannot close temporary Cascade config");
        if (rename(name.data(), configPath.c_str()) != 0)
            throw std::runtime_error("Cannot replace Cascade config: " + configPath.string());
        const int directory = open(configPath.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory >= 0)
        {
            fsync(directory);
            close(directory);
        }
        success = true;
    }
    catch (...)
    {
        if (!success) close(descriptor);
        unlink(name.data());
        throw;
    }
}
} // namespace

std::string PluginPaths::ConfigPath()
{
    if (const char *configured = std::getenv("CASCADE_CONFIG_FILE"); configured && *configured)
        return LexicalAbsolute(Expanded(configured));
    if (const char *configHome = std::getenv("XDG_CONFIG_HOME"); configHome && *configHome)
        return LexicalAbsolute(Expanded(configHome) / "cascade" / "config.json");
    const char *home = std::getenv("HOME");
    if (!home || !*home) home = std::getenv("USERPROFILE");
    return LexicalAbsolute((home && *home ? fs::path(home) : fs::path(".")) / ".config" / "cascade" / "config.json");
}

std::string PluginPaths::RuntimePrefix()
{
    if (const char *configured = std::getenv("CASCADE_PREFIX"); configured && *configured)
        return Absolute(Expanded(configured));
    Dl_info libraryInfo{};
    if (dladdr(reinterpret_cast<void *>(&PluginPaths::RuntimePrefix), &libraryInfo) != 0 && libraryInfo.dli_fname)
        return Absolute(fs::path(libraryInfo.dli_fname).parent_path().parent_path());
    const char *home = std::getenv("HOME");
    return Absolute(home && *home ? fs::path(home) / ".local" : fs::path("."));
}

std::string PluginPaths::CanonicalPrefix(const std::string &path) { return Absolute(Expanded(path)); }

std::vector<std::string> PluginPaths::Unique(const std::vector<std::string> &paths)
{
    std::vector<std::string> result;
    std::set<std::string> seen;
    for (const auto &path : paths)
    {
        const std::string canonical = CanonicalPrefix(path);
        if (seen.insert(canonical).second) result.push_back(canonical);
    }
    return result;
}

PluginConfig PluginPaths::LoadConfig(const std::string &path)
{
    const fs::path configPath = path.empty() ? ConfigPath() : LexicalAbsolute(Expanded(path));
    if (!fs::exists(configPath)) return {};
    std::ifstream input(configPath);
    if (!input) throw std::runtime_error("Cannot open Cascade config: " + configPath.string());
    nlohmann::json config;
    input >> config;
    if (!config.is_object() || config.value("schema", 0) != 1)
        throw std::runtime_error("Unsupported Cascade config schema: " + configPath.string());
    const auto entries = config.value("plugin_prefixes", nlohmann::json::array());
    if (!entries.is_array()) throw std::runtime_error("Cascade config plugin_prefixes must be a list");
    PluginConfig result;
    std::set<std::string> seen;
    for (const auto &entry : entries)
    {
        std::string value;
        bool enabled = true;
        if (entry.is_string())
            value = entry.get<std::string>();
        else if (entry.is_object())
        {
            if (!entry.contains("path") || !entry.at("path").is_string())
                throw std::runtime_error("Plugin prefix path must be a non-empty string");
            value = entry.at("path").get<std::string>();
            if (entry.contains("enabled") && !entry.at("enabled").is_boolean())
                throw std::runtime_error("Plugin prefix enabled flag must be boolean");
            enabled = entry.value("enabled", true);
        }
        else
            throw std::runtime_error("Invalid plugin prefix entry");
        if (value.empty()) throw std::runtime_error("Plugin prefix path must be a non-empty string");
        value = CanonicalPrefix(value);
        if (seen.insert(value).second) result.Prefixes.push_back({std::move(value), enabled});
    }
    return result;
}

std::vector<std::string> PluginPaths::ConfiguredPrefixes(const std::string &path)
{
    std::vector<std::string> result;
    for (const auto &entry : LoadConfig(path).Prefixes)
        if (entry.Enabled) result.push_back(entry.Path);
    return result;
}

bool PluginPaths::AddPrefix(const std::string &prefixValue, const std::string &path)
{
    const fs::path configPath = path.empty() ? ConfigPath() : LexicalAbsolute(Expanded(path));
    const std::string prefix = CanonicalPrefix(prefixValue);
    ConfigLock lock(configPath);
    auto config = LoadConfig(configPath.string());
    for (auto &entry : config.Prefixes)
    {
        if (entry.Path != prefix) continue;
        if (entry.Enabled) return false;
        entry.Enabled = true;
        AtomicWriteConfig(config, configPath);
        return true;
    }
    config.Prefixes.push_back({prefix, true});
    AtomicWriteConfig(config, configPath);
    return true;
}

bool PluginPaths::RemovePrefix(const std::string &prefixValue, const std::string &path)
{
    const fs::path configPath = path.empty() ? ConfigPath() : LexicalAbsolute(Expanded(path));
    const std::string prefix = CanonicalPrefix(prefixValue);
    ConfigLock lock(configPath);
    auto config = LoadConfig(configPath.string());
    const auto original = config.Prefixes.size();
    config.Prefixes.erase(std::remove_if(config.Prefixes.begin(), config.Prefixes.end(),
                                         [&](const PluginPrefix &entry) { return entry.Path == prefix; }),
                          config.Prefixes.end());
    if (config.Prefixes.size() == original) return false;
    AtomicWriteConfig(config, configPath);
    return true;
}

PluginLayout PluginPaths::Layout(const std::string &prefixValue)
{
    PluginLayout layout;
    layout.Prefix = CanonicalPrefix(prefixValue);
    const fs::path prefix(layout.Prefix);
    layout.Cpp = (prefix / "lib" / "cascade" / "plugin").string();
    layout.Python = (prefix / "lib" / "cascade" / "pyplugin").string();
    layout.Include = (prefix / "include" / "cascade" / "plugin").string();
    layout.TrustStore = (prefix / "share" / "cascade" / "trusted_keys").string();
    return layout;
}

std::vector<PluginLayout> PluginPaths::RuntimeLayouts()
{
    std::vector<PluginLayout> candidates;
    const char *trust = std::getenv("CASCADE_PLUGIN_TRUST_STORE");
    const char *cpp = std::getenv("CASCADE_PLUGIN_DIR");
    const char *python = std::getenv("CASCADE_PYPLUGIN_DIR");
    if ((cpp && *cpp) || (python && *python))
    {
        PluginLayout environment = Layout(RuntimePrefix());
        environment.Prefix = "environment";
        environment.Source = "environment";
        environment.Cpp = cpp && *cpp ? CanonicalPrefix(cpp) : "";
        environment.Python = python && *python ? CanonicalPrefix(python) : "";
        if (trust && *trust) environment.TrustStore = CanonicalPrefix(trust);
        candidates.push_back(std::move(environment));
    }
    for (const auto &prefix : ConfiguredPrefixes())
    {
        auto layout = Layout(prefix);
        layout.Source = "config";
        if (trust && *trust) layout.TrustStore = CanonicalPrefix(trust);
        candidates.push_back(std::move(layout));
    }
    auto runtime = Layout(RuntimePrefix());
    runtime.Source = "cascade";
    if (trust && *trust) runtime.TrustStore = CanonicalPrefix(trust);
    candidates.push_back(std::move(runtime));

    std::vector<PluginLayout> result;
    std::set<std::string> seen;
    for (auto &layout : candidates)
    {
        const std::string key = layout.Cpp + '\n' + layout.Python + '\n' + layout.TrustStore;
        if (seen.insert(key).second) result.push_back(std::move(layout));
    }
    return result;
}

std::vector<std::string> PluginPaths::Roots(const std::string &language)
{
    std::vector<std::string> roots;
    for (const auto &layout : RuntimeLayouts()) roots.push_back(language == "python" ? layout.Python : layout.Cpp);
    return Unique(roots);
}

std::string PluginPaths::TrustStoreForRoot(const std::string &pluginRoot)
{
    if (const char *configured = std::getenv("CASCADE_PLUGIN_TRUST_STORE"); configured && *configured)
        return CanonicalPrefix(configured);
    fs::path prefix = fs::path(pluginRoot);
    for (int level = 0; level < 3 && prefix.has_parent_path(); ++level) prefix = prefix.parent_path();
    return Absolute(prefix / "share" / "cascade" / "trusted_keys");
}
