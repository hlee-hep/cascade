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
inline constexpr bool g_isParamValueV =
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
        ParamValue &m_Value;

      public:
        explicit ParamProxy(ParamValue &v) : m_Value(v) {}
        template <typename T> T Get() const { return std::get<T>(m_Value); }
        template <typename T, typename = std::enable_if_t<std::is_same_v<T, double> || std::is_same_v<T, int> || std::is_same_v<T, std::string>>> operator T()
        {
            return std::get<T>(m_Value);
        }
        template <typename T> ParamProxy &operator=(const T &newVal)
        {
            m_Value = newVal;
            return *this;
        }
        std::string TypeName() const
        {
            return std::visit([](auto &&x) { return ::TypeName<std::decay_t<decltype(x)>>(); }, m_Value);
        }
    };

    class ConstParamProxy
    {
        const ParamValue &m_Value;

      public:
        explicit ConstParamProxy(const ParamValue &v) : m_Value(v) {}
        template <typename T> T Get() const { return std::get<T>(m_Value); }
        template <typename T, typename = std::enable_if_t<std::is_same_v<T, double> || std::is_same_v<T, int> || std::is_same_v<T, std::string>>>
        operator T() const
        {
            return std::get<T>(m_Value);
        }
        std::string TypeName() const
        {
            return std::visit([](auto &&x) { return ::TypeName<std::decay_t<decltype(x)>>(); }, m_Value);
        }
    };

  private:
    std::unordered_map<std::string, ParamValue> m_RawValues;
    std::unordered_map<std::string, std::string> m_Descriptions;

    ParamValue ConvertFromYaml_(const YAML::Node &val);
    ParamValue ConvertFromPy_(const py::object &obj) const;
    json ToJsonInternal_() const;
    YAML::Node ToYamlInternal_() const;

    template <typename T> void UpdateExisting_(const std::string &key, const T &value, const std::string &desc = "")
    {
        static_assert(g_isParamValueV<T>, "[ParamManager] Unsupported type.");
        auto it = m_RawValues.find(key);
        if (it == m_RawValues.end()) throw std::runtime_error("ParamManager: key '" + key + "' is not registered.");
        it->second = value;
        if (!desc.empty()) m_Descriptions[key] = desc;
    }

  public:
    ParamManager();

    // ===== Registration & access =====
    template <typename T> void Register(const std::string &key, const T &value, const std::string &desc = "")
    {
        static_assert(g_isParamValueV<T>, "[ParamManager] Unsupported type.");
        if (m_RawValues.count(key)) throw std::runtime_error("ParamManager: key already registered: " + key);
        m_RawValues[key] = value;
        if (!desc.empty()) m_Descriptions[key] = desc;
    }

    template <typename T> void Set(const std::string &key, const T &value) { UpdateExisting_(key, value); }

    template <typename T> T Get(const std::string &key) const
    {
        static_assert(g_isParamValueV<T>, "[ParamManager] Requested type not in ParamValue.");
        auto it = m_RawValues.find(key);
        if (it == m_RawValues.end()) throw std::runtime_error("ParamManager: key not found: " + key);
        if constexpr (std::is_same_v<T, ParamValue>)
            return it->second;
        else
            return std::get<T>(it->second);
    }

    bool Has(const std::string &key) const { return m_RawValues.count(key) > 0; }

    ParamProxy operator[](const std::string &key)
    {
        auto it = m_RawValues.find(key);
        if (it == m_RawValues.end()) throw std::runtime_error("ParamManager: key '" + key + "' is not registered.");
        return ParamProxy(it->second);
    }

    ConstParamProxy operator[](const std::string &key) const
    {
        auto it = m_RawValues.find(key);
        if (it == m_RawValues.end()) throw std::runtime_error("ParamManager: key '" + key + "' is not registered.");
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

    void SetParamVariant(const std::string &key, const ParamValue &v) { UpdateExisting_(key, v); }
    void RegisterCommon();

    const auto &RawValues() const { return m_RawValues; }
    const auto &Descriptions() const { return m_Descriptions; }
};
