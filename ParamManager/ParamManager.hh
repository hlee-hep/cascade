#pragma once
#include "Logger.hh"

#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <vector>

using json = nlohmann::json;

// ===== Mixed element / vector =====
using MixedElement = std::variant<long long, double, std::string, bool>;
using MixedVector = std::vector<MixedElement>;

// ===== ParamValue =====
using ParamValue =
    std::variant<std::monostate, bool, int, long, long long, double, std::string, std::vector<int>, std::vector<double>, std::vector<std::string>, MixedVector>;

class IAnalysisModule;

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
        ParamManager &m_Manager;
        std::string m_Key;

      public:
        ParamProxy(ParamManager &manager, std::string key) : m_Manager(manager), m_Key(std::move(key)) {}
        template <typename T> T Get() const { return m_Manager.Get<T>(m_Key); }
        template <typename T, typename = std::enable_if_t<std::is_same_v<T, double> || std::is_same_v<T, int> || std::is_same_v<T, std::string>>> operator T()
        {
            return m_Manager.Get<T>(m_Key);
        }
        template <typename T> ParamProxy &operator=(const T &newVal)
        {
            m_Manager.Set(m_Key, newVal);
            return *this;
        }
        std::string TypeName() const { return m_Manager.TypeOf(m_Key); }
    };

    class ConstParamProxy
    {
        const ParamManager &m_Manager;
        std::string m_Key;

      public:
        ConstParamProxy(const ParamManager &manager, std::string key) : m_Manager(manager), m_Key(std::move(key)) {}
        template <typename T> T Get() const { return m_Manager.Get<T>(m_Key); }
        template <typename T, typename = std::enable_if_t<std::is_same_v<T, double> || std::is_same_v<T, int> || std::is_same_v<T, std::string>>>
        operator T() const
        {
            return m_Manager.Get<T>(m_Key);
        }
        std::string TypeName() const { return m_Manager.TypeOf(m_Key); }
    };

  private:
    mutable std::recursive_mutex m_Mutex;
    bool m_Frozen = false;
    std::unordered_map<std::string, ParamValue> m_RawValues;
    std::shared_ptr<const std::unordered_map<std::string, ParamValue>> m_FrozenValues;
    std::unordered_map<std::string, std::string> m_Descriptions;

    ParamValue ConvertFromYaml_(const YAML::Node &val);
    ParamValue ConvertFromJson_(const json &val) const;
    ParamValue CoerceToRegisteredType_(const std::string &key, const ParamValue &value) const;
    json ToJsonInternal_() const;
    YAML::Node ToYamlInternal_() const;
    void Freeze();
    void Thaw();
    friend class IAnalysisModule;

    template <typename T> void UpdateExisting_(const std::string &key, const T &value, const std::string &desc = "")
    {
        std::lock_guard<std::recursive_mutex> lock(m_Mutex);
        static_assert(g_isParamValueV<T>, "[ParamManager] Unsupported type.");
        if (m_Frozen) throw std::runtime_error("ParamManager: parameters are immutable during module execution.");
        auto it = m_RawValues.find(key);
        if (it == m_RawValues.end()) throw std::runtime_error("ParamManager: key '" + key + "' is not registered.");
        it->second = CoerceToRegisteredType_(key, ParamValue(value));
        if (!desc.empty()) m_Descriptions[key] = desc;
    }

  public:
    ParamManager();
    ParamManager(const ParamManager &other);
    ParamManager &operator=(const ParamManager &other);
    ParamManager(ParamManager &&other);
    ParamManager &operator=(ParamManager &&other);
    // ===== Registration & access =====
    template <typename T> void Register(const std::string &key, const T &value, const std::string &desc = "")
    {
        std::lock_guard<std::recursive_mutex> lock(m_Mutex);
        static_assert(g_isParamValueV<T>, "[ParamManager] Unsupported type.");
        if (m_Frozen) throw std::runtime_error("ParamManager: parameters are immutable during module execution.");
        if (m_RawValues.count(key)) throw std::runtime_error("ParamManager: key already registered: " + key);
        m_RawValues[key] = value;
        if (!desc.empty()) m_Descriptions[key] = desc;
    }

    template <typename T> void Set(const std::string &key, const T &value) { UpdateExisting_(key, value); }

    template <typename T> T Get(const std::string &key) const
    {
        const auto frozen = std::atomic_load_explicit(&m_FrozenValues, std::memory_order_acquire);
        if (frozen)
        {
            static_assert(g_isParamValueV<T>, "[ParamManager] Requested type not in ParamValue.");
            const auto it = frozen->find(key);
            if (it == frozen->end()) throw std::runtime_error("ParamManager: key not found: " + key);
            if constexpr (std::is_same_v<T, ParamValue>)
                return it->second;
            else
                return std::get<T>(it->second);
        }
        std::lock_guard<std::recursive_mutex> lock(m_Mutex);
        static_assert(g_isParamValueV<T>, "[ParamManager] Requested type not in ParamValue.");
        auto it = m_RawValues.find(key);
        if (it == m_RawValues.end()) throw std::runtime_error("ParamManager: key not found: " + key);
        if constexpr (std::is_same_v<T, ParamValue>)
            return it->second;
        else
            return std::get<T>(it->second);
    }

    bool Has(const std::string &key) const
    {
        const auto frozen = std::atomic_load_explicit(&m_FrozenValues, std::memory_order_acquire);
        if (frozen) return frozen->count(key) > 0;
        std::lock_guard<std::recursive_mutex> lock(m_Mutex);
        return m_RawValues.count(key) > 0;
    }
    std::string TypeOf(const std::string &key) const
    {
        const auto frozen = std::atomic_load_explicit(&m_FrozenValues, std::memory_order_acquire);
        if (frozen)
        {
            const auto it = frozen->find(key);
            if (it == frozen->end()) throw std::runtime_error("ParamManager: key not found: " + key);
            return std::visit([](const auto &value) { return ::TypeName<std::decay_t<decltype(value)>>(); }, it->second);
        }
        std::lock_guard<std::recursive_mutex> lock(m_Mutex);
        auto it = m_RawValues.find(key);
        if (it == m_RawValues.end()) throw std::runtime_error("ParamManager: key not found: " + key);
        return std::visit([](const auto &value) { return ::TypeName<std::decay_t<decltype(value)>>(); }, it->second);
    }

    ParamProxy operator[](const std::string &key)
    {
        const auto frozen = std::atomic_load_explicit(&m_FrozenValues, std::memory_order_acquire);
        if (frozen)
        {
            if (!frozen->count(key)) throw std::runtime_error("ParamManager: key '" + key + "' is not registered.");
            return ParamProxy(*this, key);
        }
        std::lock_guard<std::recursive_mutex> lock(m_Mutex);
        auto it = m_RawValues.find(key);
        if (it == m_RawValues.end()) throw std::runtime_error("ParamManager: key '" + key + "' is not registered.");
        return ParamProxy(*this, key);
    }

    ConstParamProxy operator[](const std::string &key) const
    {
        const auto frozen = std::atomic_load_explicit(&m_FrozenValues, std::memory_order_acquire);
        if (frozen)
        {
            if (!frozen->count(key)) throw std::runtime_error("ParamManager: key '" + key + "' is not registered.");
            return ConstParamProxy(*this, key);
        }
        std::lock_guard<std::recursive_mutex> lock(m_Mutex);
        auto it = m_RawValues.find(key);
        if (it == m_RawValues.end()) throw std::runtime_error("ParamManager: key '" + key + "' is not registered.");
        return ConstParamProxy(*this, key);
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
    void SetParamsFromJSON(const std::string &document);

    void SetParamVariant(const std::string &key, const ParamValue &v) { UpdateExisting_(key, v); }
    void RegisterCommon();

    std::unordered_map<std::string, ParamValue> RawValues() const
    {
        const auto frozen = std::atomic_load_explicit(&m_FrozenValues, std::memory_order_acquire);
        if (frozen) return *frozen;
        std::lock_guard<std::recursive_mutex> lock(m_Mutex);
        return m_RawValues;
    }
    std::unordered_map<std::string, std::string> Descriptions() const
    {
        std::lock_guard<std::recursive_mutex> lock(m_Mutex);
        return m_Descriptions;
    }
};
