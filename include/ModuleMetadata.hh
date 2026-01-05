#pragma once
#include <string>
#include <vector>

struct ModuleMetadata
{
    std::string Name;
    std::string Version;
    std::string Summary;
    std::vector<std::string> Tags;
};
