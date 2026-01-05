#include <fstream>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

void RootMacroExample(const char *json_path = "") {
  std::cout << "JSON path: " << json_path << std::endl;
  if (std::string(json_path).empty()) {
    std::cerr << "No JSON path provided.\n";
    return;
  }

  std::ifstream in(json_path);
  if (!in) {
    std::cerr << "Failed to open JSON file: " << json_path << "\n";
    return;
  }

  json params;
  in >> params;

  std::string yaml_path = params.value("_yaml_path", "");
  int n = params.value("n", 0);
  std::string mode = params.value("mode", "default");

  std::cout << "yaml: " << yaml_path << "\n";
  std::cout << "n: " << n << "\n";
  std::cout << "mode: " << mode << "\n";

  // ... analysis logic ...
}
