#pragma once
#include <string>
#include <vector>

namespace Cascade
{
struct CutSpec
{
    std::string Name;
    std::string Expression;
};
struct HistogramSpec
{
    std::string Name;
    std::string Expression;
    int Bins = 1;
    double Min = 0.0;
    double Max = 1.0;
};
struct ConfigValidationResult
{
    std::vector<std::string> Errors;
    bool Valid() const { return Errors.empty(); }
    void ThrowIfInvalid(const std::string &path) const;
};
// Structural validation only; the user's backend interprets expressions.
ConfigValidationResult PreflightCutConfig(const std::string &path);
ConfigValidationResult PreflightHistogramConfig(const std::string &path);
std::vector<CutSpec> LoadCuts(const std::string &path);
std::vector<HistogramSpec> LoadHistograms(const std::string &path);
// Selection preserves the caller's order. An empty selection selects no cuts.
std::vector<CutSpec> SelectCuts(const std::vector<CutSpec> &cuts, const std::vector<std::string> &names);
void WriteCuts(const std::string &path, const std::vector<CutSpec> &cuts);
void WriteHistograms(const std::string &path, const std::vector<HistogramSpec> &histograms);
}
