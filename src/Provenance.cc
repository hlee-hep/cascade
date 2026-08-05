#include "Provenance.hh"
#include "AnalysisModuleRegistry.hh"

#include "PluginABI.hh"
#include "Version.hh"
#include "sha256.hh"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <ctime>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <tuple>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace
{
struct ActiveRun
{
    std::string InstanceName;
    std::string ModuleName;
    std::string Language;
    std::string StartedAt;
    bool Isolated = false;
    std::string CacheSourceManifest;
    std::vector<fs::path> Inputs;
    std::optional<PluginOrigin> Plugin;
};

std::mutex g_ProvenanceMutex;
std::map<std::string, ActiveRun> g_ActiveRuns;
std::map<std::string, ModuleRunManifest> g_ModuleRuns;
std::map<std::string, std::string> g_LastRunByInstance;
std::atomic<unsigned long long> g_WorkflowCounter{0};
constexpr std::uintmax_t kMaximumModuleManifestBytes = 16 * 1024 * 1024;

struct FileIdentity
{
    std::uintmax_t Device = 0;
    std::uintmax_t Inode = 0;
    std::uintmax_t Size = 0;
    long long ModifiedSeconds = 0;
    long long ModifiedNanoseconds = 0;
    long long ChangedSeconds = 0;
    long long ChangedNanoseconds = 0;

    bool operator<(const FileIdentity &other) const
    {
        return std::tie(Device, Inode, Size, ModifiedSeconds, ModifiedNanoseconds, ChangedSeconds, ChangedNanoseconds) <
               std::tie(other.Device, other.Inode, other.Size, other.ModifiedSeconds, other.ModifiedNanoseconds,
                        other.ChangedSeconds, other.ChangedNanoseconds);
    }
    bool operator==(const FileIdentity &other) const
    {
        return Device == other.Device && Inode == other.Inode && Size == other.Size &&
               ModifiedSeconds == other.ModifiedSeconds && ModifiedNanoseconds == other.ModifiedNanoseconds &&
               ChangedSeconds == other.ChangedSeconds && ChangedNanoseconds == other.ChangedNanoseconds;
    }
};

std::mutex g_ArtifactHashMutex;
std::map<FileIdentity, std::string> g_ArtifactHashes;

FileIdentity IdentityOf(const struct stat &metadata)
{
    FileIdentity identity;
    identity.Device = static_cast<std::uintmax_t>(metadata.st_dev);
    identity.Inode = static_cast<std::uintmax_t>(metadata.st_ino);
    identity.Size = static_cast<std::uintmax_t>(metadata.st_size);
#if defined(__APPLE__)
    identity.ModifiedSeconds = metadata.st_mtimespec.tv_sec;
    identity.ModifiedNanoseconds = metadata.st_mtimespec.tv_nsec;
    identity.ChangedSeconds = metadata.st_ctimespec.tv_sec;
    identity.ChangedNanoseconds = metadata.st_ctimespec.tv_nsec;
#else
    identity.ModifiedSeconds = metadata.st_mtim.tv_sec;
    identity.ModifiedNanoseconds = metadata.st_mtim.tv_nsec;
    identity.ChangedSeconds = metadata.st_ctim.tv_sec;
    identity.ChangedNanoseconds = metadata.st_ctim.tv_nsec;
#endif
    return identity;
}

std::size_t ArtifactHashCacheEntries()
{
    static const std::size_t limit = []()
    {
        const char *configured = std::getenv("CASCADE_PROVENANCE_HASH_CACHE_ENTRIES");
        if (!configured || !*configured) return static_cast<std::size_t>(1024);
        const std::string value(configured);
        if (value.front() == '-') throw std::runtime_error("CASCADE_PROVENANCE_HASH_CACHE_ENTRIES must be non-negative");
        std::size_t parsed = 0;
        const auto result = std::stoull(value, &parsed);
        if (parsed != value.size()) throw std::runtime_error("CASCADE_PROVENANCE_HASH_CACHE_ENTRIES must be non-negative");
        return static_cast<std::size_t>(result);
    }();
    return limit;
}

bool SensitiveKey(std::string key)
{
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    static const std::vector<std::string> patterns = {
        "password", "passwd", "secret", "token", "credential", "private_key", "api_key"};
    return std::any_of(patterns.begin(), patterns.end(),
                       [&](const std::string &pattern) { return key.find(pattern) != std::string::npos; });
}

json SanitizedParameters(const std::string &parametersJson)
{
    json source;
    try
    {
        source = json::parse(parametersJson);
    }
    catch (...)
    {
        return json{{"unparsed", parametersJson}};
    }
    if (!source.is_object()) return source;

    json result = json::object();
    for (auto iterator = source.begin(); iterator != source.end(); ++iterator)
    {
        json value = iterator.value();
        if (value.is_object() && value.contains("value")) value = value["value"];
        result[iterator.key()] = SensitiveKey(iterator.key()) ? json("***") : value;
    }
    return result;
}

std::string AbsoluteString(const fs::path &path)
{
    if (path.empty()) return {};
    std::error_code error;
    const auto absolute = fs::absolute(path, error);
    return (error ? path.lexically_normal() : absolute.lexically_normal()).string();
}

std::string HashFile(const fs::path &path)
{
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const int descriptor = open(path.c_str(), flags);
    if (descriptor < 0) throw std::system_error(errno, std::generic_category(), "Cannot read artifact for hashing");
    struct stat before{};
    if (fstat(descriptor, &before) != 0 || !S_ISREG(before.st_mode))
    {
        const int error = errno ? errno : EINVAL;
        close(descriptor);
        throw std::system_error(error, std::generic_category(), "Cannot inspect artifact for hashing");
    }
    const FileIdentity identity = IdentityOf(before);
    const std::size_t cacheLimit = ArtifactHashCacheEntries();
    if (cacheLimit > 0)
    {
        std::lock_guard<std::mutex> lock(g_ArtifactHashMutex);
        const auto cached = g_ArtifactHashes.find(identity);
        if (cached != g_ArtifactHashes.end())
        {
            close(descriptor);
            return cached->second;
        }
    }
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    if (!context)
    {
        close(descriptor);
        throw std::runtime_error("Cannot allocate SHA-256 context.");
    }
    if (EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1)
    {
        EVP_MD_CTX_free(context);
        close(descriptor);
        throw std::runtime_error("Cannot initialize SHA-256 context.");
    }
    std::array<char, 1024 * 1024> buffer{};
    while (true)
    {
        const ssize_t count = read(descriptor, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0)
        {
            EVP_MD_CTX_free(context);
            const int error = errno;
            close(descriptor);
            throw std::system_error(error, std::generic_category(), "Failed while hashing artifact");
        }
        if (count == 0) break;
        if (EVP_DigestUpdate(context, buffer.data(), static_cast<std::size_t>(count)) != 1)
        {
            EVP_MD_CTX_free(context);
            close(descriptor);
            throw std::runtime_error("Cannot update SHA-256 digest.");
        }
    }
    struct stat after{};
    if (fstat(descriptor, &after) != 0 || !(IdentityOf(after) == identity))
    {
        EVP_MD_CTX_free(context);
        close(descriptor);
        throw std::runtime_error("Artifact changed while it was being hashed: " + path.string());
    }
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digestLength = 0;
    if (EVP_DigestFinal_ex(context, digest, &digestLength) != 1)
    {
        EVP_MD_CTX_free(context);
        close(descriptor);
        throw std::runtime_error("Cannot finalize SHA-256 digest.");
    }
    EVP_MD_CTX_free(context);
    close(descriptor);
    std::ostringstream output;
    for (unsigned int index = 0; index < digestLength; ++index)
        output << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[index]);
    const std::string result = output.str();
    if (cacheLimit > 0)
    {
        std::lock_guard<std::mutex> lock(g_ArtifactHashMutex);
        while (g_ArtifactHashes.size() >= cacheLimit && !g_ArtifactHashes.empty()) g_ArtifactHashes.erase(g_ArtifactHashes.begin());
        g_ArtifactHashes[identity] = result;
    }
    return result;
}

enum class ArtifactHashMode
{
    Full,
    Metadata,
    None
};

enum class InputHashMode
{
    Metadata,
    Full,
    Auto
};

const char *ArtifactHashModeName(ArtifactHashMode mode)
{
    switch (mode)
    {
    case ArtifactHashMode::Full:
        return "full";
    case ArtifactHashMode::Metadata:
        return "metadata";
    case ArtifactHashMode::None:
        return "none";
    }
    return "none";
}

InputHashMode ConfiguredInputHashMode()
{
    const char *configured = std::getenv("CASCADE_INPUT_HASH_MODE");
    const std::string value = configured && *configured ? configured : "metadata";
    if (value == "metadata") return InputHashMode::Metadata;
    if (value == "full") return InputHashMode::Full;
    if (value == "auto") return InputHashMode::Auto;
    throw std::runtime_error("CASCADE_INPUT_HASH_MODE must be metadata, full, or auto");
}

ArtifactHashMode InputArtifactHashMode(const fs::path &path)
{
    switch (ConfiguredInputHashMode())
    {
    case InputHashMode::Metadata:
        return ArtifactHashMode::Metadata;
    case InputHashMode::Full:
        return ArtifactHashMode::Full;
    case InputHashMode::Auto:
    {
        std::error_code error;
        if (fs::is_regular_file(path, error) && !error && fs::file_size(path, error) <= 64ULL * 1024ULL * 1024ULL &&
            !error)
            return ArtifactHashMode::Full;
        return ArtifactHashMode::Metadata;
    }
    }
    return ArtifactHashMode::Metadata;
}

ArtifactHashMode ConfiguredArtifactHashMode()
{
    const char *configured = std::getenv("CASCADE_PROVENANCE_HASH_MODE");
    const std::string value = configured && *configured ? configured : "full";
    if (value == "full") return ArtifactHashMode::Full;
    if (value == "metadata") return ArtifactHashMode::Metadata;
    if (value == "none") return ArtifactHashMode::None;
    throw std::runtime_error("CASCADE_PROVENANCE_HASH_MODE must be full, metadata, or none");
}

long long ModifiedAt(const fs::path &path)
{
    std::error_code error;
    const auto time = fs::last_write_time(path, error);
    return error ? 0 : time.time_since_epoch().count();
}

ArtifactProvenance CaptureArtifact(const fs::path &source, const std::string &recordedPath,
                                   ArtifactHashMode hashMode)
{
    ArtifactProvenance artifact;
    artifact.Path = recordedPath;
    artifact.HashMode = ArtifactHashModeName(hashMode);
    std::error_code error;
    const auto status = fs::symlink_status(source, error);
    if (error || !fs::exists(status))
    {
        artifact.Kind = source.string().find("://") != std::string::npos ? "uri" : "missing";
        return artifact;
    }
    artifact.Exists = true;
    struct stat identityMetadata{};
    if (lstat(source.c_str(), &identityMetadata) == 0)
    {
        const FileIdentity identity = IdentityOf(identityMetadata);
        artifact.Device = identity.Device;
        artifact.Inode = identity.Inode;
        artifact.ModifiedSeconds = identity.ModifiedSeconds;
        artifact.ModifiedNanoseconds = identity.ModifiedNanoseconds;
        artifact.ChangedSeconds = identity.ChangedSeconds;
        artifact.ChangedNanoseconds = identity.ChangedNanoseconds;
    }
    if (fs::is_symlink(status))
    {
        artifact.Kind = "symlink";
        const auto target = fs::read_symlink(source, error).string();
        if (!error)
        {
            artifact.Size = target.size();
            if (hashMode != ArtifactHashMode::None) artifact.Sha256 = Sha256(target);
        }
        return artifact;
    }
    if (fs::is_regular_file(status))
    {
        artifact.Kind = "file";
        artifact.Size = fs::file_size(source);
        if (hashMode == ArtifactHashMode::Full) artifact.Sha256 = HashFile(source);
        return artifact;
    }
    if (!fs::is_directory(status))
    {
        artifact.Kind = "other";
        return artifact;
    }

    artifact.Kind = "directory";
    if (hashMode == ArtifactHashMode::None) return artifact;
    std::vector<fs::path> entries;
    for (fs::recursive_directory_iterator iterator(source), end; iterator != end; ++iterator)
        entries.push_back(iterator->path());
    std::sort(entries.begin(), entries.end());
    std::ostringstream fingerprint;
    for (const auto &entry : entries)
    {
        const auto relative = entry.lexically_relative(source).generic_string();
        const auto entryStatus = fs::symlink_status(entry, error);
        if (error) continue;
        if (fs::is_directory(entryStatus))
        {
            fingerprint << "d\0" << relative << '\0' << ModifiedAt(entry) << '\0';
        }
        else if (fs::is_symlink(entryStatus))
        {
            const auto target = fs::read_symlink(entry, error).string();
            if (error) continue;
            artifact.Size += target.size();
            fingerprint << "l\0" << relative << '\0' << Sha256(target) << '\0' << target.size() << '\0';
        }
        else if (fs::is_regular_file(entryStatus))
        {
            const auto size = fs::file_size(entry);
            artifact.Size += size;
            fingerprint << "f\0" << relative << '\0';
            if (hashMode == ArtifactHashMode::Full) fingerprint << HashFile(entry);
            fingerprint << '\0' << size << '\0' << ModifiedAt(entry) << '\0';
        }
    }
    artifact.Sha256 = Sha256(fingerprint.str());
    return artifact;
}

json ArtifactJson(const ArtifactProvenance &artifact)
{
    return {{"path", artifact.Path},
            {"kind", artifact.Kind},
            {"exists", artifact.Exists},
            {"size", artifact.Size},
            {"hash_mode", artifact.HashMode.empty() ? json(nullptr) : json(artifact.HashMode)},
            {"identity",
             artifact.Exists
                 ? json{{"device", artifact.Device},
                        {"inode", artifact.Inode},
                        {"mtime_seconds", artifact.ModifiedSeconds},
                        {"mtime_nanoseconds", artifact.ModifiedNanoseconds},
                        {"ctime_seconds", artifact.ChangedSeconds},
                        {"ctime_nanoseconds", artifact.ChangedNanoseconds}}
                 : json(nullptr)},
            {"sha256", artifact.Sha256.empty() ? json(nullptr) : json(artifact.Sha256)}};
}

ArtifactProvenance ArtifactFromJson(const json &value)
{
    ArtifactProvenance artifact;
    artifact.Path = value.value("path", "");
    artifact.Kind = value.value("kind", "");
    if (value.contains("hash_mode") && value["hash_mode"].is_string())
        artifact.HashMode = value["hash_mode"].get<std::string>();
    artifact.Exists = value.value("exists", false);
    artifact.Size = value.value("size", static_cast<std::uintmax_t>(0));
    const auto identity = value.value("identity", json(nullptr));
    if (identity.is_object())
    {
        artifact.Device = identity.value("device", static_cast<std::uintmax_t>(0));
        artifact.Inode = identity.value("inode", static_cast<std::uintmax_t>(0));
        artifact.ModifiedSeconds = identity.value("mtime_seconds", static_cast<std::int64_t>(0));
        artifact.ModifiedNanoseconds = identity.value("mtime_nanoseconds", static_cast<std::int64_t>(0));
        artifact.ChangedSeconds = identity.value("ctime_seconds", static_cast<std::int64_t>(0));
        artifact.ChangedNanoseconds = identity.value("ctime_nanoseconds", static_cast<std::int64_t>(0));
    }
    if (value.contains("sha256") && value["sha256"].is_string()) artifact.Sha256 = value["sha256"].get<std::string>();
    return artifact;
}

json RuntimeJson(const RuntimeProvenance &runtime)
{
    return {{"cascade_version", runtime.CascadeVersion},
            {"plugin_abi_version", runtime.PluginAbiVersion},
            {"plugin_abi_tag", runtime.PluginAbiTag},
            {"root_version", runtime.RootVersion},
            {"language", runtime.Language}};
}

json PluginOriginJson(const std::optional<PluginOrigin> &origin)
{
    if (!origin) return nullptr;
    return {{"package", origin->Package},
            {"trust", ToString(origin->Trust)},
            {"manifest_path", origin->ManifestPath},
            {"manifest_sha256", origin->ManifestSha256},
            {"artifact_sha256", origin->ArtifactSha256},
            {"signer_fingerprint", origin->SignerFingerprint.empty() ? json(nullptr) : json(origin->SignerFingerprint)}};
}

std::optional<PluginOrigin> PluginOriginFromJson(const json &value)
{
    if (!value.is_object()) return std::nullopt;
    PluginOrigin origin;
    origin.Package = value.value("package", "");
    origin.ManifestPath = value.value("manifest_path", "");
    origin.ManifestSha256 = value.value("manifest_sha256", "");
    origin.ArtifactSha256 = value.value("artifact_sha256", "");
    if (value.contains("signer_fingerprint") && value["signer_fingerprint"].is_string())
        origin.SignerFingerprint = value["signer_fingerprint"].get<std::string>();
    origin.Trust = value.value("trust", "Verified") == "Signed" ? PluginTrustStatus::Signed : PluginTrustStatus::Verified;
    return origin;
}

RuntimeProvenance RuntimeFromJson(const json &value)
{
    RuntimeProvenance runtime;
    runtime.CascadeVersion = value.value("cascade_version", "");
    runtime.PluginAbiVersion = value.value("plugin_abi_version", 0);
    runtime.PluginAbiTag = value.value("plugin_abi_tag", "");
    runtime.RootVersion = value.value("root_version", "");
    runtime.Language = value.value("language", "");
    return runtime;
}

ModuleStatus StatusFromString(const std::string &value)
{
    for (const auto status : {ModuleStatus::Pending, ModuleStatus::Initializing, ModuleStatus::Running,
                              ModuleStatus::Finalizing, ModuleStatus::Done, ModuleStatus::Skipped,
                              ModuleStatus::Interrupted, ModuleStatus::Failed})
        if (value == ToString(status)) return status;
    return ModuleStatus::Pending;
}

ModulePhase PhaseFromString(const std::string &value)
{
    for (const auto phase : {ModulePhase::None, ModulePhase::Init, ModulePhase::Check, ModulePhase::Execute,
                             ModulePhase::Finalize, ModulePhase::Commit})
        if (value == ToString(phase)) return phase;
    return ModulePhase::None;
}

void AtomicWrite(const fs::path &path, const std::string &content)
{
    if (path.empty()) throw std::invalid_argument("Provenance manifest path cannot be empty.");
    if (!path.parent_path().empty()) fs::create_directories(path.parent_path());
    const fs::path parent = path.parent_path().empty() ? fs::current_path() : path.parent_path();
    std::string pattern = (parent / (path.filename().string() + ".tmp.XXXXXX")).string();
    std::vector<char> temporary(pattern.begin(), pattern.end());
    temporary.push_back('\0');
    const int descriptor = mkstemp(temporary.data());
    if (descriptor < 0) throw std::system_error(errno, std::generic_category(), "Cannot create provenance temporary file");
    bool descriptorOpen = true;
    try
    {
        std::size_t offset = 0;
        while (offset < content.size())
        {
            const ssize_t written = write(descriptor, content.data() + offset, content.size() - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0)
                throw std::system_error(errno ? errno : EIO, std::generic_category(), "Cannot write provenance manifest");
            offset += static_cast<std::size_t>(written);
        }
        if (fchmod(descriptor, 0600) != 0 || fsync(descriptor) != 0)
            throw std::system_error(errno, std::generic_category(), "Cannot flush provenance manifest");
        const int closeResult = close(descriptor);
        descriptorOpen = false;
        if (closeResult != 0)
            throw std::system_error(errno, std::generic_category(), "Cannot close provenance manifest");
        if (rename(temporary.data(), path.c_str()) != 0)
            throw std::system_error(errno, std::generic_category(), "Cannot publish provenance manifest");
        const int directory = open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
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
} // namespace

std::string ModuleRunManifest::ToJSON(int indent) const
{
    json inputs = json::array();
    for (const auto &artifact : Inputs)
        inputs.push_back(ArtifactJson(artifact));
    json outputs = json::array();
    for (const auto &artifact : Outputs)
        outputs.push_back(ArtifactJson(artifact));
    const json metadata = {{"name", Metadata.Name},
                           {"version", Metadata.Version},
                           {"summary", Metadata.Summary},
                           {"tags", Metadata.Tags}};
    const json document = {
        {"schema", "cascade.module-run"},
        {"schema_version", SchemaVersion},
        {"run_id", RunId},
        {"module", {{"instance", InstanceName}, {"name", ModuleName}, {"metadata", metadata}}},
        {"runtime", RuntimeJson(Runtime)},
        {"plugin", PluginOriginJson(Plugin)},
        {"identity", {{"code_hash", CodeHash}, {"snapshot_hash", SnapshotHash}}},
        {"parameters", SanitizedParameters(ParametersJson)},
        {"timing", {{"started_at", StartedAt}, {"finished_at", FinishedAt}}},
        {"directories", {{"output", OutputDirectory}, {"cache", CacheDirectory}}},
        {"execution",
         {{"isolated", Isolated},
          {"cache_hit", CacheHit},
          {"dry_run", DryRun},
          {"cache_decision", CacheDecision},
          {"cache_reason", CacheReason},
          {"cache_source_manifest", CacheSourceManifest.empty() ? json(nullptr) : json(CacheSourceManifest)}}},
        {"result", {{"status", ToString(Status)}, {"phase", ToString(Phase)}, {"message", Message}}},
        {"artifacts", {{"inputs", inputs}, {"outputs", outputs}}},
        {"manifest_path", ManifestPath}};
    return document.dump(indent);
}

std::string WorkflowRunManifest::ToJSON(int indent) const
{
    json nodes = json::array();
    for (const auto &node : Nodes)
        nodes.push_back({{"name", node.Name},
                         {"status", node.Status},
                         {"message", node.Message},
                         {"dependencies", node.Dependencies},
                         {"module_run_id", node.ModuleRunId.empty() ? json(nullptr) : json(node.ModuleRunId)},
                         {"module_manifest", node.ModuleManifestPath.empty() ? json(nullptr) : json(node.ModuleManifestPath)}});
    json links = json::array();
    for (const auto &link : DataLinks)
        links.push_back({{"from", link.FromNode}, {"to", link.ToNode}, {"label", link.Label}});
    const json document = {{"schema", "cascade.workflow-run"},
                           {"schema_version", SchemaVersion},
                           {"run_id", RunId},
                           {"timing", {{"started_at", StartedAt}, {"finished_at", FinishedAt}}},
                           {"runtime", RuntimeJson(Runtime)},
                           {"execution", {{"fail_fast", FailFast}, {"succeeded", Succeeded}}},
                           {"dag", {{"nodes", nodes}, {"data_links", links}}},
                           {"module_manifests", ModuleManifestPaths},
                           {"manifest_path", ManifestPath}};
    return document.dump(indent);
}

void ProvenanceRecorder::BeginModuleRun(const std::string &runId, const std::string &instanceName,
                                        const std::string &moduleName, const std::string &language, bool isolated)
{
    std::lock_guard<std::mutex> lock(g_ProvenanceMutex);
    g_ActiveRuns[runId] = {instanceName, moduleName, language, NowUTC(), isolated, {}, {}, std::nullopt};
}

void ProvenanceRecorder::TrackInput(const std::string &runId, const fs::path &path)
{
    if (runId.empty()) throw std::runtime_error("Cannot track an input outside an active module run.");
    std::lock_guard<std::mutex> lock(g_ProvenanceMutex);
    auto iterator = g_ActiveRuns.find(runId);
    if (iterator == g_ActiveRuns.end()) throw std::runtime_error("Cannot track an input for an unknown run: " + runId);
    if (std::find(iterator->second.Inputs.begin(), iterator->second.Inputs.end(), path) == iterator->second.Inputs.end())
        iterator->second.Inputs.push_back(path);
}

std::string ProvenanceRecorder::InputSnapshotState(const std::string &runId)
{
    std::vector<fs::path> inputs;
    {
        std::lock_guard<std::mutex> lock(g_ProvenanceMutex);
        const auto iterator = g_ActiveRuns.find(runId);
        if (iterator == g_ActiveRuns.end())
            throw std::runtime_error("Cannot snapshot inputs for an unknown run: " + runId);
        inputs = iterator->second.Inputs;
    }
    std::sort(inputs.begin(), inputs.end());
    json state = json::array();
    for (const auto &input : inputs)
        state.push_back(ArtifactJson(CaptureArtifact(input, AbsoluteString(input), InputArtifactHashMode(input))));
    return state.dump();
}

void ProvenanceRecorder::SetCacheSource(const std::string &runId, const std::string &manifestPath)
{
    std::lock_guard<std::mutex> lock(g_ProvenanceMutex);
    const auto iterator = g_ActiveRuns.find(runId);
    if (iterator != g_ActiveRuns.end()) iterator->second.CacheSourceManifest = manifestPath;
}

void ProvenanceRecorder::SetPluginOrigin(const std::string &runId, const std::optional<PluginOrigin> &origin)
{
    std::lock_guard<std::mutex> lock(g_ProvenanceMutex);
    const auto iterator = g_ActiveRuns.find(runId);
    if (iterator != g_ActiveRuns.end()) iterator->second.Plugin = origin;
}

ModuleRunManifest ProvenanceRecorder::BuildModuleRun(
    const std::string &runId, const ModuleMetadata &metadata, const std::string &codeHash,
    const std::string &snapshotHash, const std::string &parametersJson, const fs::path &outputDirectory,
    const fs::path &cacheDirectory, const RunResult &result,
    const std::vector<std::pair<fs::path, fs::path>> &stagedOutputs, const std::string &manifestPath)
{
    ActiveRun active;
    {
        std::lock_guard<std::mutex> lock(g_ProvenanceMutex);
        const auto iterator = g_ActiveRuns.find(runId);
        if (iterator != g_ActiveRuns.end())
            active = iterator->second;
        else
            active = {"", metadata.Name, "cpp", NowUTC(), false, {}, {}, std::nullopt};
    }

    ModuleRunManifest manifest;
    manifest.RunId = runId;
    manifest.InstanceName = active.InstanceName;
    manifest.ModuleName = active.ModuleName.empty() ? metadata.Name : active.ModuleName;
    manifest.Metadata = metadata;
    manifest.Runtime = Runtime(active.Language.empty() ? "cpp" : active.Language);
    manifest.Plugin = active.Plugin ? active.Plugin : AnalysisModuleRegistry::Get().GetPluginOrigin(manifest.ModuleName);
    manifest.CodeHash = codeHash;
    manifest.SnapshotHash = snapshotHash;
    manifest.ParametersJson = parametersJson;
    manifest.StartedAt = active.StartedAt;
    manifest.FinishedAt = NowUTC();
    manifest.OutputDirectory = AbsoluteString(outputDirectory);
    manifest.CacheDirectory = AbsoluteString(cacheDirectory);
    manifest.Isolated = active.Isolated;
    manifest.CacheHit = result.Status == ModuleStatus::Skipped && result.Message == "snapshot already cached";
    manifest.DryRun = result.Status == ModuleStatus::Skipped && result.Message == "dry_run enabled";
    manifest.CacheDecision = result.CacheDecision;
    manifest.CacheReason = result.CacheReason;
    manifest.CacheSourceManifest = active.CacheSourceManifest;
    manifest.Status = result.Status;
    manifest.Phase = result.Phase;
    manifest.Message = result.Message;
    manifest.ManifestPath = AbsoluteString(manifestPath);

    const ArtifactHashMode outputHashMode = ConfiguredArtifactHashMode();
    for (const auto &input : active.Inputs)
        manifest.Inputs.push_back(CaptureArtifact(input, input.string(), InputArtifactHashMode(input)));
    for (const auto &[finalPath, stagedPath] : stagedOutputs)
    {
        std::error_code error;
        auto relative = fs::relative(finalPath, outputDirectory, error);
        manifest.Outputs.push_back(
            CaptureArtifact(stagedPath, error ? finalPath.string() : relative.generic_string(), outputHashMode));
    }
    return manifest;
}

void ProvenanceRecorder::WriteModuleRun(const ModuleRunManifest &manifest, const fs::path &path)
{
    AtomicWrite(path, manifest.ToJSON());
}

ModuleRunManifest ProvenanceRecorder::LoadModuleRun(const fs::path &path)
{
    std::error_code sizeError;
    const auto manifestSize = fs::file_size(path, sizeError);
    if (sizeError) throw std::runtime_error("Cannot inspect provenance manifest: " + path.string());
    if (manifestSize > kMaximumModuleManifestBytes)
        throw std::runtime_error("Provenance manifest exceeds the 16 MiB limit: " + path.string());
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Cannot read provenance manifest: " + path.string());
    json value;
    input >> value;
    if (value.value("schema", "") != "cascade.module-run")
        throw std::runtime_error("Not a Cascade module provenance manifest: " + path.string());
    if (value.value("schema_version", 0) != 1)
        throw std::runtime_error("Unsupported Cascade module provenance schema version: " + path.string());

    ModuleRunManifest manifest;
    manifest.SchemaVersion = value.value("schema_version", 0);
    manifest.RunId = value.value("run_id", "");
    const auto module = value.value("module", json::object());
    manifest.InstanceName = module.value("instance", "");
    manifest.ModuleName = module.value("name", "");
    const auto metadata = module.value("metadata", json::object());
    manifest.Metadata.Name = metadata.value("name", "");
    manifest.Metadata.Version = metadata.value("version", "");
    manifest.Metadata.Summary = metadata.value("summary", "");
    manifest.Metadata.Tags = metadata.value("tags", std::vector<std::string>{});
    manifest.Runtime = RuntimeFromJson(value.value("runtime", json::object()));
    manifest.Plugin = PluginOriginFromJson(value.value("plugin", json(nullptr)));
    const auto identity = value.value("identity", json::object());
    manifest.CodeHash = identity.value("code_hash", "");
    manifest.SnapshotHash = identity.value("snapshot_hash", "");
    manifest.ParametersJson = value.value("parameters", json::object()).dump();
    const auto timing = value.value("timing", json::object());
    manifest.StartedAt = timing.value("started_at", "");
    manifest.FinishedAt = timing.value("finished_at", "");
    const auto directories = value.value("directories", json::object());
    manifest.OutputDirectory = directories.value("output", "");
    manifest.CacheDirectory = directories.value("cache", "");
    const auto execution = value.value("execution", json::object());
    manifest.Isolated = execution.value("isolated", false);
    manifest.CacheHit = execution.value("cache_hit", false);
    manifest.DryRun = execution.value("dry_run", false);
    manifest.CacheDecision = execution.value("cache_decision", manifest.CacheHit ? "hit" : "not_checked");
    manifest.CacheReason = execution.value("cache_reason", "");
    if (execution.contains("cache_source_manifest") && execution["cache_source_manifest"].is_string())
        manifest.CacheSourceManifest = execution["cache_source_manifest"].get<std::string>();
    const auto result = value.value("result", json::object());
    manifest.Status = StatusFromString(result.value("status", "Pending"));
    manifest.Phase = PhaseFromString(result.value("phase", "None"));
    manifest.Message = result.value("message", "");
    const auto artifacts = value.value("artifacts", json::object());
    for (const auto &artifact : artifacts.value("inputs", json::array()))
        manifest.Inputs.push_back(ArtifactFromJson(artifact));
    for (const auto &artifact : artifacts.value("outputs", json::array()))
        manifest.Outputs.push_back(ArtifactFromJson(artifact));
    manifest.ManifestPath = value.value("manifest_path", AbsoluteString(path));
    return manifest;
}

bool ProvenanceRecorder::ValidateCachedRun(const fs::path &path, const std::string &expectedSnapshotHash,
                                           const fs::path &outputDirectory, std::string *reason)
{
    auto fail = [&](const std::string &message)
    {
        if (reason) *reason = message;
        return false;
    };
    if (path.empty() || !fs::is_regular_file(path)) return fail("cached provenance manifest is missing");
    ModuleRunManifest manifest;
    try
    {
        manifest = LoadModuleRun(path);
    }
    catch (const std::exception &error)
    {
        return fail(std::string("cached provenance manifest is invalid: ") + error.what());
    }
    if (manifest.Status != ModuleStatus::Done) return fail("cached provenance does not describe a successful run");
    if (manifest.SnapshotHash != expectedSnapshotHash) return fail("cached provenance snapshot hash does not match");

    std::error_code error;
    const fs::path root = fs::weakly_canonical(fs::absolute(outputDirectory), error);
    if (error) return fail("configured output directory cannot be resolved");
    const fs::path recordedRoot = fs::weakly_canonical(fs::absolute(manifest.OutputDirectory), error);
    if (error || recordedRoot != root) return fail("cached provenance belongs to a different output directory");

    for (const auto &recorded : manifest.Outputs)
    {
        const fs::path relative(recorded.Path);
        if (relative.empty() || relative.is_absolute() || relative.has_root_path())
            return fail("cached provenance contains an invalid output path");
        const fs::path unresolved = (root / relative).lexically_normal();
        const fs::path resolvedParent = fs::weakly_canonical(unresolved.parent_path(), error);
        if (error) return fail("cached output parent cannot be resolved: " + recorded.Path);
        const fs::path containedParent = resolvedParent.lexically_relative(root);
        if (resolvedParent != root && (containedParent.empty() || *containedParent.begin() == ".."))
            return fail("cached output escapes the configured output directory: " + recorded.Path);
        const fs::path candidate = resolvedParent / unresolved.filename();
        struct stat metadata{};
        if (lstat(candidate.c_str(), &metadata) == 0 && recorded.Inode != 0)
        {
            const FileIdentity identity = IdentityOf(metadata);
            if (identity.Device == recorded.Device && identity.Inode == recorded.Inode &&
                identity.Size == recorded.Size && identity.ModifiedSeconds == recorded.ModifiedSeconds &&
                identity.ModifiedNanoseconds == recorded.ModifiedNanoseconds &&
                identity.ChangedSeconds == recorded.ChangedSeconds &&
                identity.ChangedNanoseconds == recorded.ChangedNanoseconds)
                continue;
        }
        ArtifactProvenance current;
        try
        {
            ArtifactHashMode validationMode = ArtifactHashMode::Full;
            if (recorded.HashMode == "metadata")
                validationMode = ArtifactHashMode::Metadata;
            else if (recorded.HashMode == "none" || (recorded.HashMode.empty() && recorded.Sha256.empty()))
                validationMode = ArtifactHashMode::None;
            current = CaptureArtifact(candidate, recorded.Path, validationMode);
        }
        catch (const std::exception &failure)
        {
            return fail("cannot validate cached output " + recorded.Path + ": " + failure.what());
        }
        if (!current.Exists || current.Kind != recorded.Kind || current.Size != recorded.Size)
            return fail("cached output metadata changed: " + recorded.Path);
        if (!recorded.Sha256.empty() && current.Sha256 != recorded.Sha256)
            return fail("cached output content changed: " + recorded.Path);
    }
    return true;
}

void ProvenanceRecorder::RefreshOutputIdentities(ModuleRunManifest &manifest, const fs::path &outputDirectory)
{
    for (auto &artifact : manifest.Outputs)
    {
        const fs::path path = outputDirectory / artifact.Path;
        struct stat metadata{};
        if (lstat(path.c_str(), &metadata) != 0)
            throw std::system_error(errno, std::generic_category(), "Cannot inspect committed output identity");
        const FileIdentity identity = IdentityOf(metadata);
        artifact.Device = identity.Device;
        artifact.Inode = identity.Inode;
        artifact.ModifiedSeconds = identity.ModifiedSeconds;
        artifact.ModifiedNanoseconds = identity.ModifiedNanoseconds;
        artifact.ChangedSeconds = identity.ChangedSeconds;
        artifact.ChangedNanoseconds = identity.ChangedNanoseconds;
    }
}

void ProvenanceRecorder::StoreModuleRun(const ModuleRunManifest &manifest)
{
    std::lock_guard<std::mutex> lock(g_ProvenanceMutex);
    const auto previous = g_LastRunByInstance.find(manifest.InstanceName);
    if (!manifest.InstanceName.empty() && previous != g_LastRunByInstance.end() && previous->second != manifest.RunId)
        g_ModuleRuns.erase(previous->second);
    g_ModuleRuns[manifest.RunId] = manifest;
    g_LastRunByInstance[manifest.InstanceName] = manifest.RunId;
    g_ActiveRuns.erase(manifest.RunId);
}

void ProvenanceRecorder::DiscardModuleRun(const std::string &runId)
{
    std::lock_guard<std::mutex> lock(g_ProvenanceMutex);
    g_ActiveRuns.erase(runId);
}

std::optional<ModuleRunManifest> ProvenanceRecorder::FindModuleRun(const std::string &runId)
{
    std::lock_guard<std::mutex> lock(g_ProvenanceMutex);
    const auto iterator = g_ModuleRuns.find(runId);
    return iterator == g_ModuleRuns.end() ? std::nullopt : std::optional<ModuleRunManifest>(iterator->second);
}

std::optional<ModuleRunManifest> ProvenanceRecorder::FindLastModuleRun(const std::string &instanceName)
{
    std::lock_guard<std::mutex> lock(g_ProvenanceMutex);
    const auto last = g_LastRunByInstance.find(instanceName);
    if (last == g_LastRunByInstance.end()) return std::nullopt;
    const auto run = g_ModuleRuns.find(last->second);
    return run == g_ModuleRuns.end() ? std::nullopt : std::optional<ModuleRunManifest>(run->second);
}

std::string ProvenanceRecorder::SuccessfulModuleManifestPath(const fs::path &outputDirectory, const std::string &runId)
{
    return AbsoluteString(outputDirectory / ".cascade" / "provenance" / "modules" / (runId + ".json"));
}

std::string ProvenanceRecorder::TerminalModuleManifestPath(const fs::path &cacheDirectory, const std::string &runId)
{
    return AbsoluteString(cacheDirectory / "provenance" / "modules" / (runId + ".json"));
}

std::string ProvenanceRecorder::DefaultWorkflowManifestPath(const std::string &runId)
{
    const char *configured = std::getenv("CASCADE_CACHE_DIR");
    fs::path root;
    if (configured && *configured)
        root = configured;
    else if (const char *home = std::getenv("HOME"); home && *home)
        root = fs::path(home) / ".cache" / "cascade";
    else
        root = ".cascade-cache";
    return AbsoluteString(root / "provenance" / "workflows" / (runId + ".json"));
}

std::string ProvenanceRecorder::MakeWorkflowRunId()
{
    const auto now = std::chrono::system_clock::now();
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    return "workflow-" + std::to_string(getpid()) + "-" + std::to_string(micros) + "-" +
           std::to_string(g_WorkflowCounter.fetch_add(1));
}

std::string ProvenanceRecorder::NowUTC()
{
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(now);
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(now - seconds).count();
    const std::time_t value = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_r(&value, &utc);
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(6) << std::setfill('0') << micros << 'Z';
    return output.str();
}

RuntimeProvenance ProvenanceRecorder::Runtime(const std::string &language)
{
    RuntimeProvenance runtime;
    runtime.CascadeVersion = CascadeVersionString();
    runtime.PluginAbiVersion = CASCADE_PLUGIN_ABI_VERSION;
    runtime.PluginAbiTag = CASCADE_ABI_TAG;
    runtime.RootVersion = ROOT_RELEASE;
    runtime.Language = language;
    return runtime;
}

std::string ProvenanceRecorder::WriteWorkflowRun(WorkflowRunManifest manifest, const fs::path &path)
{
    const fs::path target = path.empty() ? fs::path(DefaultWorkflowManifestPath(manifest.RunId)) : path;
    manifest.ManifestPath = AbsoluteString(target);
    AtomicWrite(target, manifest.ToJSON());
    return manifest.ManifestPath;
}
