#pragma once
#include "Logger.hh"
#include "ParamManager.hh"
#include "sha256.hh"
#include <string>

class SnapshotHasher
{
  public:
    inline static std::string ComputeSerialized(const ParamManager &pm, const std::string &moduleName,
                                                const std::string &codeVersion, const std::string &analysisState,
                                                const std::string &executionState = "",
                                                const std::string &pluginArtifactHash = "",
                                                const std::string &inputState = "")
    {
        const json document = {
            {"schema", "cascade.snapshot"},
            {"schema_version", 5},
            {"module", moduleName},
            {"parameters", json::parse(pm.DumpJSON())},
            {"analysis_state", analysisState},
            {"execution_state", executionState},
            {"code_version", codeVersion},
            {"plugin_artifact_sha256", pluginArtifactHash},
            {"tracked_inputs", inputState},
        };
        const std::string serialized = document.dump();
        LOG_DEBUG("SnapshotHasher", serialized);
        return Sha256(serialized);
    }
};
