#pragma once
#include "AnalysisManager.hh"
#include "Logger.hh"
#include "ParamManager.hh"
#include "sha256.hh"
#include <memory>
#include <string>
#include <vector>
#include <yaml-cpp/yaml.h>

class SnapshotHasher
{
  public:
    inline static std::string ComputeSerialized(const ParamManager &pm, const std::string &moduleName,
                                                const std::string &codeVersion, const std::string &analysisState,
                                                const std::string &executionState = "",
                                                const std::string &pluginArtifactHash = "")
    {
        const json document = {
            {"schema", "cascade.snapshot"},
            {"schema_version", 2},
            {"module", moduleName},
            {"parameters", json::parse(pm.DumpJSON())},
            {"analysis_state", analysisState},
            {"execution_state", executionState},
            {"code_version", codeVersion},
            {"plugin_artifact_sha256", pluginArtifactHash},
        };
        const std::string serialized = document.dump();
        LOG_DEBUG("SnapshotHasher", serialized);
        return Sha256(serialized);
    }

    inline static std::string Compute(const ParamManager &pm, const std::map<std::string, std::unique_ptr<AnalysisManager>> &mgrs,
                                      const std::string &moduleName, const std::string &codeVersion,
                                      const std::string &executionState = "", const std::string &pluginArtifactHash = "")
    {
        std::stringstream analysisState;
        for (const auto &[mn, uam] : mgrs)
        {
            analysisState << mn;
            const AnalysisManager *am = uam.get();
            analysisState << am->SnapshotState();
        }
        return ComputeSerialized(pm, moduleName, codeVersion, analysisState.str(), executionState, pluginArtifactHash);
    }
};
