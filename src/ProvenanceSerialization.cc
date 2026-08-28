#include "Provenance.hh"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <fcntl.h>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace
{
constexpr std::uintmax_t kMaximumModuleManifestBytes = 16 * 1024 * 1024;

std::string AbsoluteString(const fs::path &path)
{
    if (path.empty()) return {};
    std::error_code error;
    const auto absolute = fs::absolute(path, error);
    return (error ? path.lexically_normal() : absolute.lexically_normal()).string();
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
            {"code_sha256", origin->CodeSha256.empty() ? origin->ArtifactSha256 : origin->CodeSha256},
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
    origin.CodeSha256 = value.value("code_sha256", origin.ArtifactSha256);
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
std::string ProvenanceRecorder::WriteWorkflowRun(WorkflowRunManifest manifest, const fs::path &path)
{
    const fs::path target = path.empty() ? fs::path(DefaultWorkflowManifestPath(manifest.RunId)) : path;
    manifest.ManifestPath = AbsoluteString(target);
    AtomicWrite(target, manifest.ToJSON());
    return manifest.ManifestPath;
}
