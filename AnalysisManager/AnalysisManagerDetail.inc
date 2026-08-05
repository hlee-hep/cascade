#pragma once

#include <cctype>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace cascade::analysis_detail
{
inline std::string SafeColumnName(std::string value)
{
    for (char &character : value)
        if (!std::isalnum(static_cast<unsigned char>(character)) && character != '_') character = '_';
    return value;
}

inline void ValidateHistogramBins(const std::vector<double> &bins, const std::string &name)
{
    if (bins.size() != 3 || !std::isfinite(bins[0]) || std::floor(bins[0]) != bins[0] || bins[0] <= 0 ||
        !std::isfinite(bins[1]) || !std::isfinite(bins[2]) || bins[2] <= bins[1])
        throw std::invalid_argument(
            "AnalysisManager: histogram '" + name + "' bins must be {integer_nbins, xmin, xmax} with a positive finite range.");
}
} // namespace cascade::analysis_detail
