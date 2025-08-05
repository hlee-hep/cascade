#pragma once

#include "LambdaManager.hh"
#include "Logger.hh"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>

#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <vector>

namespace py = pybind11;
using json = nlohmann::json;

using ParamValueLeaf = std::variant<bool, int, long, long long, double, std::string, std::vector<int>, std::vector<double>, std::vector<std::string>>;

using ParamValue = ParamValueLeaf;

template <typename T>
inline constexpr bool is_param_value_v =
    std::disjunction_v<std::is_same<T, ParamValue>, std::is_same<T, bool>, std::is_same<T, int>, std::is_same<T, long>, std::is_same<T, long long>,
                       std::is_same<T, double>, std::is_same<T, std::string>, std::is_same<T, std::vector<int>>, std::is_same<T, std::vector<double>>,
                       std::is_same<T, std::vector<std::string>>>;

template <typename T> static std::string TypeName()
{
    if constexpr (std::is_same_v<T, std::string>)
        return "string";
    else if constexpr (std::is_same_v<T, bool>)
        return "bool";
    else if constexpr (std::is_same_v<T, int>)
        return "int";
    else if constexpr (std::is_same_v<T, long>)
        return "long";
    else if constexpr (std::is_same_v<T, long long>)
        return "long long";
    else if constexpr (std::is_same_v<T, double>)
        return "double";
    else if constexpr (std::is_same_v<T, std::vector<int>>)
        return "vector<int>";
    else if constexpr (std::is_same_v<T, std::vector<double>>)
        return "vector<double>";
    else if constexpr (std::is_same_v<T, std::vector<std::string>>)
        return "vector<string>";
    else
        return "unknown";
}

class __attribute__((visibility("default"))) ParamManager
{
  private:
    std::unordered_map<std::string, ParamValue> rawValues_;
    std::unordered_map<std::string, std::string> descriptions_;

    template <typename T> void SetInternal(const std::string &key, const T &value, const std::string &desc = "", bool overwrite = true)
    {
        static_assert(is_param_value_v<T>, "[ParamManager] Unsupported type for ParamValue variant. Add it to ParamValue alias first.");
        if (!overwrite && rawValues_.count(key)) throw std::runtime_error("Param '" + key + "' already exists. Use SetParam() to overwrite.");
        rawValues_[key] = value;
        if (!desc.empty()) descriptions_[key] = desc;
    }

    ParamValue ConvertFromPy(const py::object &obj) const
    {
        if (py::isinstance<py::bool_>(obj)) return obj.cast<bool>();
        if (py::isinstance<py::int_>(obj)) return static_cast<long long>(obj.cast<long long>());
        if (py::isinstance<py::float_>(obj)) return obj.cast<double>();
        if (py::isinstance<py::str>(obj)) return obj.cast<std::string>();

        if (py::isinstance<py::list>(obj))
        {
            if (py::len(obj) == 0) return std::vector<std::string>{};
            py::object first = obj[0];
            if (py::isinstance<py::str>(first)) return obj.cast<std::vector<std::string>>();
            if (py::isinstance<py::int_>(first)) return obj.cast<std::vector<int>>();
            if (py::isinstance<py::float_>(first)) return obj.cast<std::vector<double>>();
        }
        throw std::runtime_error("Unsupported py::object for ParamManager::ConvertFromPy");
    }

    ParamValue ConvertFromYAML(const YAML::Node &val)
    {
        if (val.IsScalar())
        {
            std::string s = val.Scalar();
            if (s == "true") return true;
            if (s == "false") return false;
            try
            {
                double d = std::stod(s);
                if (std::floor(d) == d)
                {
                    if (d <= std::numeric_limits<int>::max())
                        return static_cast<int>(d);
                    else if (d <= std::numeric_limits<long>::max())
                        return static_cast<long>(d);
                    else
                        return static_cast<long long>(d);
                }
                else
                {
                    return d;
                }
            }
            catch (...)
            {
            }
            return s;
        }
        else if (val.IsSequence())
        {
            if (val.size() == 0) return std::vector<std::string>{};
            try
            {
                return val.as<std::vector<int>>();
            }
            catch (...)
            {
            }
            try
            {
                return val.as<std::vector<double>>();
            }
            catch (...)
            {
            }
            try
            {
                return val.as<std::vector<std::string>>();
            }
            catch (...)
            {
            }
            throw std::runtime_error("Unsupported sequence type in YAML.");
        }
        else if (val.IsMap())
        {
            json j;
            for (const auto &kv : val)
                j[kv.first.as<std::string>()] = kv.second.as<std::string>();
            return j.dump();
        }
        throw std::runtime_error("Unsupported YAML node type.");
    }

  public:
    ParamManager() { RegisterCommon(); }

    template <typename T> void Register(const std::string &key, const T &value, const std::string &desc = "")
    {
        SetInternal(key, value, desc, /*overwrite*/ false);
    }

    template <typename T> void Set(const std::string &key, const T &value) { SetInternal(key, value); }

    void SetParamFromPy(const std::string &key, const py::object &obj, const std::string &desc = "")
    {
        ParamValue v = ConvertFromPy(obj);
        std::visit([&](auto &&val) { SetInternal(key, val, desc); }, v);
    }

    void SetParamsFromYAML(const YAML::Node &node)
    {
        for (const auto &it : node)
        {
            const auto &key = it.first.as<std::string>();
            const auto &val = it.second;
            ParamValue v = ConvertFromYAML(val);
            std::visit([&](auto &&x) { SetInternal(key, x); }, v);
        }
    }

    void SetParamVariant(const std::string &key, const ParamValue &v) { rawValues_[key] = v; }

    template <typename T> T Get(const std::string &key) const
    {
        static_assert(is_param_value_v<T>, "[ParamManager] Requested type not in ParamValue variant.");
        auto it = rawValues_.find(key);
        if (it == rawValues_.end()) throw std::runtime_error("Param '" + key + "' not found.");

        if constexpr (std::is_same_v<T, ParamValue>)
        {
            return it->second;
        }
        else
        {
            return std::get<T>(it->second);
        }
    }

    template <typename T> T GetOr(const std::string &key, const T &fallback) const { return rawValues_.count(key) ? Get<T>(key) : fallback; }

    bool Has(const std::string &key) const { return rawValues_.count(key) > 0; }

    std::string DumpJSON() const
    {
        json j;
        for (const auto &[k, v] : rawValues_)
        {
            json entry;
            entry["description"] = descriptions_.count(k) ? descriptions_.at(k) : "";
            std::visit(
                [&](auto &&val)
                {
                    entry["value"] = val;
                    entry["type"] = TypeName<std::decay_t<decltype(val)>>();
                },
                v);
            j[k] = entry;
        }
        return j.dump();
    }

    std::string PrintParameters() const
    {
        json j = json::parse(DumpJSON());
        return j.dump(4);
    }

    YAML::Node ToYAMLNode() const
    {
        YAML::Node node;
        for (const auto &[k, v] : rawValues_)
        {
            std::visit([&](auto &&val) { node[k] = val; }, v);
        }
        return node;
    }

    void RegisterCommon()
    {
        Register("dry_run", false, "simulate execution only");
        Register("force_run", false, "force execution");
    }

    void RegisterCommonLambdas(LambdaManager *lm) const;
};
