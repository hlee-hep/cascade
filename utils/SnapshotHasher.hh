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
                                                const std::string &executionState = "")
    {
        std::stringstream ss;

        ss << "[Module]" << moduleName;
        ss << "[Params]" << pm.DumpJSON();
        ss << "[AM]" << analysisState;
        ss << "[Execution]" << executionState;
        ss << "[Code]" << codeVersion;

        LOG_DEBUG("SnapshotHasher", ss.str());
        return Sha256(ss.str());
    }

    inline static std::string Compute(const ParamManager &pm, const std::map<std::string, std::unique_ptr<AnalysisManager>> &mgrs,
                                      const std::string &moduleName, const std::string &codeVersion, const std::string &executionState = "")
    {
        std::stringstream analysisState;
        for (const auto &[mn, uam] : mgrs)
        {
            analysisState << mn;
            const AnalysisManager *am = uam.get();
            analysisState << am->SnapshotState();
        }
        return ComputeSerialized(pm, moduleName, codeVersion, analysisState.str(), executionState);
    }
};
