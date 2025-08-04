#pragma once
#include "LambdaManager.hh"
#include "json.hpp"
#include <any>
#include <functional>
#include <iostream>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <yaml-cpp/yaml.h>

namespace py = pybind11;
using json = nlohmann::json;

class __attribute__((visibility("default"))) ParamManager
{
  private:
    std::unordered_map<std::string, std::string> values_;
    std::unordered_map<std::string, std::any> rawValues_;
    std::unordered_map<std::string, std::string> descriptions_;
    static std::unordered_map<std::type_index, std::function<void(ParamManager *, const std::string &, const std::any &)>> casters;

  public:
    ParamManager() = default;

    template <typename T> void Register(const std::string &key, const T &value, const std::string &desc = "")
    {
        using RealT = std::decay_t<T>;

        if constexpr (std::is_same_v<RealT, const char *>)
        {
            Register<std::string>(key, std::string(value), desc);
            return;
        }

        try
        {
            std::ostringstream oss;
            oss << value;
            values_[key] = oss.str();
            rawValues_[key] = value;
            descriptions_[key] = desc;

            // std::cout << "[OK] Register '" << key << "' as " <<
            // typeid(T).name() << std::endl;
        }
        catch (const std::exception &e)
        {
            // std::cerr << "[EXCEPTION] Register failed for '" << key << "': "
            // << e.what() << std::endl;
            throw;
        }
    }

    void RegisterCommon();

    template <typename T> void Set(const std::string &key, const T &value)
    {
        if constexpr (std::is_convertible_v<T, std::string> || std::is_arithmetic_v<T>)
        {
            std::ostringstream oss;
            oss << value;
            values_[key] = oss.str();
        }
        else
        {
            values_[key] = typeid(T).name();
        }
        rawValues_[key] = value;
    }

    template <typename T> T Get(const std::string &key) const
    {
        auto it = rawValues_.find(key);
        if (it == rawValues_.end()) throw std::runtime_error("Param '" + key + "' not found in rawValues.");

        if (it->second.type() == typeid(std::string))
        {
            std::istringstream iss(std::any_cast<std::string>(it->second));
            T val;
            iss >> val;
            if (iss.fail()) throw std::runtime_error("Failed to convert param '" + key + "' to requested type.");
            return val;
        }
        else
        {
            return std::any_cast<T>(it->second);
        }
    }

    std::string DumpJSON() const
    {
        std::ostringstream oss;
        oss << "{";
        bool first = true;
        for (const auto &[k, v] : values_)
        {
            if (!first) oss << ",";
            first = false;
            oss << "\"" << k << "\": {"
                << "\"value\": \"" << v << "\", "
                << "\"description\": \"" << EscapeJSON(descriptions_.at(k)) << "\"}";
        }
        oss << "}";
        return oss.str();
    }

    std::string PrintParameters() const
    {
        json j = json::parse(DumpJSON());
        return j.dump(4);
    }

    YAML::Node ToYAMLNode() const
    {
        YAML::Node node;
        for (const auto &[k, v] : values_)
            node[k] = v;
        return node;
    }

    void RegisterCommonLambdas(LambdaManager *lm) const;
    void SetParamFromAny(const std::string &key, const std::any &value);

    template <typename T> static void RegisterAnyCaster()
    {
        casters[typeid(T)] = [](ParamManager *pm, const std::string &key, const std::any &val)
        {
            if constexpr (std::is_convertible_v<T, std::string> || std::is_arithmetic_v<T>)
            {
                std::ostringstream oss;
                oss << std::any_cast<T>(val);
                pm->values_[key] = oss.str();
            }
            else
            {
                pm->values_[key] = typeid(T).name();
            }
            pm->rawValues_[key] = std::any_cast<T>(val);
        };
    }

    std::any ConvertFromPy(const py::object &obj) const;
    std::any GetRawAny(const std::string &key) const { return rawValues_.at(key); }

  private:
    static std::string EscapeJSON(const std::string &str)
    {
        std::ostringstream oss;
        for (char c : str)
        {
            switch (c)
            {
            case '"':
                oss << "\\\"";
                break;
            case '\\':
                oss << "\\\\";
                break;
            case '\n':
                oss << "\\n";
                break;
            default:
                oss << c;
                break;
            }
        }
        return oss.str();
    }
};
