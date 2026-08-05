#pragma once

#include "ModuleMetadata.hh"
#include "ModuleRun.hh"
#include "PluginTrust.hh"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct ArtifactProvenance
{
    std::string Path;
    std::string Kind;
    std::string Sha256;
    std::uintmax_t Size = 0;
    std::uintmax_t Device = 0;
    std::uintmax_t Inode = 0;
    std::int64_t ModifiedSeconds = 0;
    std::int64_t ModifiedNanoseconds = 0;
    std::int64_t ChangedSeconds = 0;
    std::int64_t ChangedNanoseconds = 0;
    bool Exists = false;
};

struct RuntimeProvenance
{
    std::string CascadeVersion;
    int PluginAbiVersion = 0;
    std::string PluginAbiTag;
    std::string RootVersion;
    std::string Language;
};

struct ModuleRunManifest
{
    int SchemaVersion = 1;
    std::string RunId;
    std::string InstanceName;
    std::string ModuleName;
    ModuleMetadata Metadata;
    RuntimeProvenance Runtime;
    std::optional<PluginOrigin> Plugin;
    std::string CodeHash;
    std::string SnapshotHash;
    std::string ParametersJson;
    std::string StartedAt;
    std::string FinishedAt;
    std::string OutputDirectory;
    std::string CacheDirectory;
    bool Isolated = false;
    bool CacheHit = false;
    bool DryRun = false;
    std::string CacheSourceManifest;
    ModuleStatus Status = ModuleStatus::Pending;
    ModulePhase Phase = ModulePhase::None;
    std::string Message;
    std::vector<ArtifactProvenance> Inputs;
    std::vector<ArtifactProvenance> Outputs;
    std::string ManifestPath;

    std::string ToJSON(int indent = 2) const;
};

struct WorkflowNodeProvenance
{
    std::string Name;
    std::string Status;
    std::string Message;
    std::vector<std::string> Dependencies;
    std::string ModuleRunId;
    std::string ModuleManifestPath;
};

struct WorkflowDataLinkProvenance
{
    std::string FromNode;
    std::string ToNode;
    std::string Label;
};

struct WorkflowRunManifest
{
    int SchemaVersion = 1;
    std::string RunId;
    std::string StartedAt;
    std::string FinishedAt;
    bool FailFast = true;
    bool Succeeded = false;
    RuntimeProvenance Runtime;
    std::vector<WorkflowNodeProvenance> Nodes;
    std::vector<WorkflowDataLinkProvenance> DataLinks;
    std::vector<std::string> ModuleManifestPaths;
    std::string ManifestPath;

    std::string ToJSON(int indent = 2) const;
};

class ProvenanceRecorder
{
  public:
    static void BeginModuleRun(const std::string &runId, const std::string &instanceName, const std::string &moduleName,
                               const std::string &language, bool isolated);
    static void TrackInput(const std::string &runId, const std::filesystem::path &path);
    static std::string InputSnapshotState(const std::string &runId);
    static void SetCacheSource(const std::string &runId, const std::string &manifestPath);
    static void SetPluginOrigin(const std::string &runId, const std::optional<PluginOrigin> &origin);

    static ModuleRunManifest BuildModuleRun(
        const std::string &runId, const ModuleMetadata &metadata, const std::string &codeHash,
        const std::string &snapshotHash, const std::string &parametersJson, const std::filesystem::path &outputDirectory,
        const std::filesystem::path &cacheDirectory, const RunResult &result,
        const std::vector<std::pair<std::filesystem::path, std::filesystem::path>> &stagedOutputs,
        const std::string &manifestPath);

    static void WriteModuleRun(const ModuleRunManifest &manifest, const std::filesystem::path &path);
    static ModuleRunManifest LoadModuleRun(const std::filesystem::path &path);
    static bool ValidateCachedRun(const std::filesystem::path &path, const std::string &expectedSnapshotHash,
                                  const std::filesystem::path &outputDirectory, std::string *reason = nullptr);
    static void RefreshOutputIdentities(ModuleRunManifest &manifest, const std::filesystem::path &outputDirectory);
    static void StoreModuleRun(const ModuleRunManifest &manifest);
    static void DiscardModuleRun(const std::string &runId);
    static std::optional<ModuleRunManifest> FindModuleRun(const std::string &runId);
    static std::optional<ModuleRunManifest> FindLastModuleRun(const std::string &instanceName);

    static std::string SuccessfulModuleManifestPath(const std::filesystem::path &outputDirectory,
                                                    const std::string &runId);
    static std::string TerminalModuleManifestPath(const std::filesystem::path &cacheDirectory,
                                                  const std::string &runId);
    static std::string DefaultWorkflowManifestPath(const std::string &runId);
    static std::string MakeWorkflowRunId();
    static std::string NowUTC();
    static RuntimeProvenance Runtime(const std::string &language);

    static std::string WriteWorkflowRun(WorkflowRunManifest manifest, const std::filesystem::path &path = {});
};
