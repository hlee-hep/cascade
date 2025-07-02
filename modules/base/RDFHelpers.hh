#pragma once
#include <cmath>
#include <functional>
#include <map>
#include <string>

namespace RDFHelpers
{

inline auto ellipticalCut(double x0, double y0, double a, double b)
{
    return [=](double x, double y)
    { return std::pow((x - x0) / a, 2) + std::pow((y - y0) / b, 2) < 1.0; };
}

} // namespace RDFHelpers
