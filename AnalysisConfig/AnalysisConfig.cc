#include "AnalysisConfig.hh"
#include <cmath>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <yaml-cpp/yaml.h>

namespace Cascade
{
namespace
{
bool Blank(const std::string &text) { return text.find_first_not_of(" \t\r\n") == std::string::npos; }
struct ParsedConfig
{
    ConfigValidationResult Report;
    std::vector<CutSpec> Cuts;
    std::vector<HistogramSpec> Histograms;
};
ParsedConfig Parse(const YAML::Node &root, bool histogram)
{
    ParsedConfig parsed;
    auto &errors = parsed.Report.Errors;
    if (!root.IsMap())
    {
        errors.push_back("document must be a map");
        return parsed;
    }
    std::set<std::string> topKeys;
    for (const auto &entry : root)
        if (!entry.first.IsScalar() || !topKeys.insert(entry.first.as<std::string>()).second)
            errors.push_back("document keys must be unique strings");
    try
    {
        if (!root["schema_version"] || root["schema_version"].as<int>() != 1)
            errors.push_back("schema_version must be 1");
    }
    catch (const YAML::Exception &) { errors.push_back("schema_version must be 1"); }
    const std::string section = histogram ? "histograms" : "cuts";
    const auto entries = root[section];
    if (!entries || !entries.IsMap())
    {
        errors.push_back(section + " must be a map");
        return parsed;
    }
    std::set<std::string> names;
    for (const auto &entry : entries)
    {
        try
        {
            const auto name = entry.first.as<std::string>();
            if (Blank(name) || !names.insert(name).second)
                throw std::invalid_argument("names must be non-empty and unique: " + name);
            const auto info = entry.second;
            if (histogram && !info.IsMap()) throw std::invalid_argument(name + " must be a map");
            if (histogram)
            {
                std::set<std::string> fields;
                for (const auto &field : info)
                    if (!field.first.IsScalar() || !fields.insert(field.first.as<std::string>()).second)
                        throw std::invalid_argument(name + " fields must be unique strings");
            }
            const auto expressionNode = histogram ? info["expr"] : info;
            if (!expressionNode || !expressionNode.IsScalar())
                throw std::invalid_argument(name + " expression must be a string");
            const auto expression = expressionNode.as<std::string>();
            if (Blank(expression) || expression.rfind("--lambda:", 0) == 0)
                throw std::invalid_argument(name + " expression must be non-empty serializable text");
            if (!histogram)
            {
                parsed.Cuts.push_back({name, expression});
                continue;
            }
            const auto bins = info["bins"];
            if (!bins || !bins.IsSequence() || bins.size() != 3)
                throw std::invalid_argument(name + " bins must be [nbins, xmin, xmax]");
            const double count = bins[0].as<double>();
            const double low = bins[1].as<double>();
            const double high = bins[2].as<double>();
            if (!std::isfinite(count) || count <= 0 || std::floor(count) != count ||
                count > std::numeric_limits<int>::max() || !std::isfinite(low) || !std::isfinite(high) || high <= low)
                throw std::invalid_argument(name + " requires positive integer bins and a finite increasing range");
            parsed.Histograms.push_back({name, expression, static_cast<int>(count), low, high});
        }
        catch (const std::exception &error) { errors.push_back(section + ": " + error.what()); }
    }
    return parsed;
}
ParsedConfig Read(const std::string &path, bool histogram)
{
    try { return Parse(YAML::LoadFile(path), histogram); }
    catch (const std::exception &error)
    {
        ParsedConfig parsed;
        parsed.Report.Errors.push_back(error.what());
        return parsed;
    }
}
void Save(const std::string &path, const YAML::Node &root, bool histogram)
{
    Parse(root, histogram).Report.ThrowIfInvalid(path);
    YAML::Emitter emitter;
    emitter.SetDoublePrecision(std::numeric_limits<double>::max_digits10);
    emitter << root;
    if (!emitter.good()) throw std::runtime_error("Cannot serialize analysis config: " + path);
    std::ofstream output(path);
    output << emitter.c_str() << '\n';
    output.close();
    if (!output) throw std::runtime_error("Cannot write analysis config: " + path);
}
}
void ConfigValidationResult::ThrowIfInvalid(const std::string &path) const
{
    if (Valid()) return;
    std::ostringstream message;
    message << "Invalid analysis config: " << path;
    for (const auto &error : Errors) message << "\n- " << error;
    throw std::invalid_argument(message.str());
}
ConfigValidationResult PreflightCutConfig(const std::string &path) { return Read(path, false).Report; }
ConfigValidationResult PreflightHistogramConfig(const std::string &path) { return Read(path, true).Report; }
std::vector<CutSpec> LoadCuts(const std::string &path)
{
    auto parsed = Read(path, false);
    parsed.Report.ThrowIfInvalid(path);
    return parsed.Cuts;
}
std::vector<HistogramSpec> LoadHistograms(const std::string &path)
{
    auto parsed = Read(path, true);
    parsed.Report.ThrowIfInvalid(path);
    return parsed.Histograms;
}
std::vector<CutSpec> SelectCuts(const std::vector<CutSpec> &cuts, const std::vector<std::string> &names)
{
    std::vector<CutSpec> selected;
    std::set<std::string> seen;
    for (const auto &name : names)
    {
        if (!seen.insert(name).second) throw std::invalid_argument("Duplicate selected cut: " + name);
        const CutSpec *found = nullptr;
        for (const auto &cut : cuts)
            if (cut.Name == name)
            {
                if (found) throw std::invalid_argument("Ambiguous cut: " + name);
                found = &cut;
            }
        if (!found) throw std::invalid_argument("Unknown cut: " + name);
        selected.push_back(*found);
    }
    return selected;
}
void WriteCuts(const std::string &path, const std::vector<CutSpec> &cuts)
{
    YAML::Node root;
    root["schema_version"] = 1;
    root["cuts"] = YAML::Node(YAML::NodeType::Map);
    std::set<std::string> names;
    for (const auto &cut : cuts)
    {
        if (!names.insert(cut.Name).second) throw std::invalid_argument("Duplicate cut: " + cut.Name);
        root["cuts"][cut.Name] = cut.Expression;
    }
    Save(path, root, false);
}
void WriteHistograms(const std::string &path, const std::vector<HistogramSpec> &histograms)
{
    YAML::Node root;
    root["schema_version"] = 1;
    root["histograms"] = YAML::Node(YAML::NodeType::Map);
    std::set<std::string> names;
    for (const auto &histogram : histograms)
    {
        if (!names.insert(histogram.Name).second) throw std::invalid_argument("Duplicate histogram: " + histogram.Name);
        auto info = root["histograms"][histogram.Name];
        info["expr"] = histogram.Expression;
        for (double value : {double(histogram.Bins), histogram.Min, histogram.Max}) info["bins"].push_back(value);
        info["bins"].SetStyle(YAML::EmitterStyle::Flow);
    }
    Save(path, root, true);
}
}
