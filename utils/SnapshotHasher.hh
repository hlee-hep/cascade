#pragma once
#include "AnalysisManager.hh"
#include "Logger.hh"
#include "ParamManager.hh"
#include "sha256.hh"
#include <memory>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <string>
#include <vector>
#include <yaml-cpp/yaml.h>

class SnapshotHasher
{
  public:
    inline static std::string Compute(const ParamManager &pm,
                                      const AnalysisManager &am,
                                      const std::string &moduleName,
                                      const std::string &codeVersion)
    {
        std::stringstream ss;

        ss << "[Module]" << moduleName;
        ss << "[Params]" << pm.DumpJSON();

        ss << "[Cuts]";
        for (const auto &[name, expr] : am.GetCutExpressions())
            ss << name << ":" << expr << ";";

        ss << "[Inputs]";
        for (const auto &f : am.GetInputFiles())
            ss << f << "|";

        ss << "[Code]" << codeVersion;
        return sha256(ss.str());
    }
};
