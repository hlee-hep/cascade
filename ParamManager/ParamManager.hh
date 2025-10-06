#pragma once
#include "Logger.hh"

#include <nlohmann/json.hpp>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <yaml-cpp/yaml.h>

#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <vector>

namespace py = pybind11;
using json = nlohmann::json;

// ===== Mixed element / vector =====
using MixedElement = std::variant<long long, double, std::string, bool>;
using MixedVector = std::vector<MixedElement>;

// ===== ParamValue =====
using ParamValue =
    std::variant<std::monostate, bool, int, long, long long, double, std::string, std::vector<int>, std::vector<double>, std::vector<std::string>, MixedVector>;

// ===== traits =====
template <typename T>
inline constexpr bool is_param_value_v =
    std::disjunction_v<std::is_same<T, ParamValue>, std::is_same<T, std::monostate>, std::is_same<T, bool>, std::is_same<T, int>, std::is_same<T, long>,
                       std::is_same<T, long long>, std::is_same<T, double>, std::is_same<T, std::string>, std::is_same<T, std::vector<int>>,
                       std::is_same<T, std::vector<double>>, std::is_same<T, std::vector<std::string>>, std::is_same<T, MixedVector>>;

template <typename T> inline std::string TypeName()
{
    if constexpr (std::is_same_v<T, std::monostate>)
        return "none";
    else if constexpr (std::is_same_v<T, std::string>)
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
    else if constexpr (std::is_same_v<T, MixedVector>)
        return "vector<mixed>";
    else
        return "unknown";
}

class __attribute__((visibility("default"))) ParamManager
{
  public:
    // ---- operator[] proxy ----
    class ParamProxy
    {
        ParamValue &value_;

      public:
        explicit ParamProxy(ParamValue &v) : value_(v) {}
        template <typename T> T get() const { return std::get<T>(value_); }
        template <typename T, typename = std::enable_if_t<std::is_same_v<T, double> || std::is_same_v<T, int> || std::is_same_v<T, std::string>>> operator T()
        {
            return std::get<T>(value_);
        }
        template <typename T> ParamProxy &operator=(const T &newVal)
        {
            value_ = newVal;
            return *this;
        }
        std::string type_name() const
        {
            return std::visit([](auto &&x) { return TypeName<std::decay_t<decltype(x)>>(); }, value_);
        }
    };

    class ConstParamProxy
    {
        const ParamValue &value_;

      public:
        explicit ConstParamProxy(const ParamValue &v) : value_(v) {}
        template <typename T> T get() const { return std::get<T>(value_); }
        template <typename T, typename = std::enable_if_t<std::is_same_v<T, double> || std::is_same_v<T, int> || std::is_same_v<T, std::string>>>
        operator T() const
        {
            return std::get<T>(value_);
        }
        std::string type_name() const
        {
            return std::visit([](auto &&x) { return TypeName<std::decay_t<decltype(x)>>(); }, value_);
        }
    };

  private:
    std::unordered_map<std::string, ParamValue> rawValues_;
    std::unordered_map<std::string, std::string> descriptions_;

    ParamValue ConvertFromYAML(const YAML::Node &val);
    ParamValue ConvertFromPy(const py::object &obj) const;
    json ToJSONInternal() const;
    YAML::Node ToYAMLInternal() const;

    template <typename T> void UpdateExisting(const std::string &key, const T &value, const std::string &desc = "")
    {
        static_assert(is_param_value_v<T>, "[ParamManager] Unsupported type.");
        auto it = rawValues_.find(key);
        if (it == rawValues_.end()) throw std::runtime_error("ParamManager: key '" + key + "' is not registered.");
        it->second = value;
        if (!desc.empty()) descriptions_[key] = desc;
    }

  public:
    ParamManager();

    // ===== Registration & access =====
    template <typename T> void Register(const std::string &key, const T &value, const std::string &desc = "")
    {
        static_assert(is_param_value_v<T>, "[ParamManager] Unsupported type.");
        if (rawValues_.count(key)) throw std::runtime_error("ParamManager: key already registered: " + key);
        rawValues_[key] = value;
        if (!desc.empty()) descriptions_[key] = desc;
    }

    template <typename T> void Set(const std::string &key, const T &value) { UpdateExisting(key, value); }

    template <typename T> T Get(const std::string &key) const
    {
        static_assert(is_param_value_v<T>, "[ParamManager] Requested type not in ParamValue.");
        auto it = rawValues_.find(key);
        if (it == rawValues_.end()) throw std::runtime_error("ParamManager: key not found: " + key);
        if constexpr (std::is_same_v<T, ParamValue>)
            return it->second;
        else
            return std::get<T>(it->second);
    }

    bool Has(const std::string &key) const { return rawValues_.count(key) > 0; }

    ParamProxy operator[](const std::string &key)
    {
        auto it = rawValues_.find(key);
        if (it == rawValues_.end()) throw std::runtime_error("ParamManager: key '" + key + "' is not registered.");
        return ParamProxy(it->second);
    }

    ConstParamProxy operator[](const std::string &key) const
    {
        auto it = rawValues_.find(key);
        if (it == rawValues_.end()) throw std::runtime_error("ParamManager: key '" + key + "' is not registered.");
        return ConstParamProxy(it->second);
    }

    // ===== I/O =====
    void LoadYAMLFile(const std::string &path);
    void SaveYAMLFile(const std::string &path) const;
    void LoadJSONFile(const std::string &path);
    void SaveJSONFile(const std::string &path) const;
    YAML::Node ToYAMLNode() const;

    std::string DumpYAML(int indent = 2) const;
    std::string DumpJSON(int indent = 2) const;

    void SetParamsFromYAML(const YAML::Node &node);
    void SetParamsFromDict(const py::dict &d);
    void SetParamFromPy(const std::string &key, const py::object &obj, const std::string &desc = "");

    void SetParamVariant(const std::string &key, const ParamValue &v) { UpdateExisting(key, v); }
    void RegisterCommon();

    const auto &RawValues() const { return rawValues_; }
    const auto &Descriptions() const { return descriptions_; }
};
