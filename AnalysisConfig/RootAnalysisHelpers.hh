#pragma once
#include "AnalysisConfig.hh"
#include <ROOT/RDataFrame.hxx>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Cascade
{
// Optional adapters: no stored graph, event loop, output, or ROOT ownership.
inline ROOT::RDF::RNode ApplyCuts(ROOT::RDF::RNode node, const std::vector<CutSpec> &cuts)
{
    for (const auto &cut : cuts) node = node.Filter(cut.Expression, cut.Name);
    return node;
}
inline ROOT::RDF::TH1DModel HistogramModel(const HistogramSpec &spec, const std::string &name = "")
{
    if (spec.Bins <= 0 || !std::isfinite(spec.Min) || !std::isfinite(spec.Max) || spec.Max <= spec.Min)
        throw std::invalid_argument("Invalid histogram bins: " + spec.Name);
    return {(name.empty() ? spec.Name : name).c_str(), "", spec.Bins, spec.Min, spec.Max};
}
// The weight is a native RDF column name; callers can Define any weight expression.
inline ROOT::RDF::RResultPtr<TH1D> BookHistogram(ROOT::RDF::RNode node, const HistogramSpec &spec,
                                             const std::string &weight = "", const std::string &name = "")
{
    auto column = spec.Expression;
    const auto columns = node.GetColumnNames();
    if (std::find(columns.begin(), columns.end(), column) == columns.end())
    {
        column = "cascade_hist_value";
        while (std::find(columns.begin(), columns.end(), column) != columns.end()) column += "_";
        node = node.Define(column, spec.Expression);
    }
    const auto model = HistogramModel(spec, name);
    return weight.empty() ? node.Histo1D(model, column) : node.Histo1D(model, column, weight);
}
}
