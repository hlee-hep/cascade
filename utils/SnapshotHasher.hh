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
    inline static std::string Compute(const ParamManager &pm, const std::map<std::string, std::unique_ptr<AnalysisManager>> &mgrs,
                                      const std::string &moduleName, const std::string &codeVersion, const std::string &executionState = "")
    {
        std::stringstream ss;

        ss << "[Module]" << moduleName;
        ss << "[Params]" << pm.DumpJSON();
        ss << "[AM]";
        for (const auto &[mn, uam] : mgrs)
        {
            ss << mn;
            const AnalysisManager *am = uam.get();
            ss << am->SnapshotState();
        }
        ss << "[Execution]" << executionState;
        ss << "[Code]" << codeVersion;

        LOG_DEBUG("SnapshotHasher", ss.str());
        return Sha256(ss.str());
    }
};
