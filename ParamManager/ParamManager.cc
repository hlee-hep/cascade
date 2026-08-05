#include "ParamManager.hh"

#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>

using json = nlohmann::json;

namespace
{
template <typename Target, typename Source> Target CheckedIntegralCast(Source value, const std::string &key)
{
    static_assert(std::is_integral_v<Target>);
    long double numeric = static_cast<long double>(value);
    if constexpr (std::is_floating_point_v<Source>)
    {
        if (!std::isfinite(value) || std::floor(value) != value)
            throw std::runtime_error("ParamManager: non-integral value for integer parameter '" + key + "'.");
    }
    if (numeric < static_cast<long double>(std::numeric_limits<Target>::lowest()) ||
        numeric > static_cast<long double>(std::numeric_limits<Target>::max()))
        throw std::runtime_error("ParamManager: integer value out of range for parameter '" + key + "'.");
    return static_cast<Target>(value);
}

bool IsNumeric(const MixedElement &value)
{
    return std::holds_alternative<long long>(value) || std::holds_alternative<double>(value);
}

double MixedAsDouble(const MixedElement &value, const std::string &key)
{
    if (const auto *integer = std::get_if<long long>(&value)) return static_cast<double>(*integer);
    if (const auto *real = std::get_if<double>(&value)) return *real;
    throw std::runtime_error("ParamManager: non-numeric list element for parameter '" + key + "'.");
}

json MixedToJSON(const MixedElement &value)
{
    return std::visit([](const auto &item) -> json { return json(item); }, value);
}

YAML::Node MixedToYAML(const MixedElement &value)
{
    return std::visit([](const auto &item) -> YAML::Node { return YAML::Node(item); }, value);
}
} // namespace

ParamManager::ParamManager() = default;

ParamManager::ParamManager(const ParamManager &other)
{
    std::lock_guard<std::recursive_mutex> lock(other.m_Mutex);
    m_RawValues = other.m_RawValues;
    m_Descriptions = other.m_Descriptions;
    m_Frozen = false;
}

ParamManager &ParamManager::operator=(const ParamManager &other)
{
    if (this == &other) return *this;
    std::scoped_lock lock(m_Mutex, other.m_Mutex);
    if (m_Frozen) throw std::runtime_error("ParamManager: parameters are immutable during module execution.");
    m_RawValues = other.m_RawValues;
    m_Descriptions = other.m_Descriptions;
    return *this;
}

ParamManager::ParamManager(ParamManager &&other)
{
    std::lock_guard<std::recursive_mutex> lock(other.m_Mutex);
    m_RawValues = std::move(other.m_RawValues);
    m_Descriptions = std::move(other.m_Descriptions);
    m_Frozen = false;
}

ParamManager &ParamManager::operator=(ParamManager &&other)
{
    if (this == &other) return *this;
    std::scoped_lock lock(m_Mutex, other.m_Mutex);
    if (m_Frozen) throw std::runtime_error("ParamManager: parameters are immutable during module execution.");
    m_RawValues = std::move(other.m_RawValues);
    m_Descriptions = std::move(other.m_Descriptions);
    return *this;
}

void ParamManager::Freeze()
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    m_Frozen = true;
}

void ParamManager::Thaw()
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    m_Frozen = false;
}

ParamValue ParamManager::ConvertFromYaml_(const YAML::Node &value)
{
    if (!value || value.IsNull()) return std::monostate{};
    if (value.IsMap()) throw std::runtime_error("ParamManager: map values are not supported.");

    if (value.IsScalar())
    {
        const std::string scalar = value.Scalar();
        if (scalar == "true" || scalar == "True" || scalar == "TRUE") return true;
        if (scalar == "false" || scalar == "False" || scalar == "FALSE") return false;

        std::size_t parsed = 0;
        try
        {
            const long long integer = std::stoll(scalar, &parsed);
            if (parsed == scalar.size()) return integer;
        }
        catch (const std::exception &)
        {
        }
        try
        {
            const double real = std::stod(scalar, &parsed);
            if (parsed == scalar.size()) return real;
        }
        catch (const std::exception &)
        {
        }
        return scalar;
    }

    if (value.IsSequence())
    {
        MixedVector result;
        result.reserve(value.size());
        for (const auto &item : value)
        {
            const ParamValue converted = ConvertFromYaml_(item);
            std::visit(
                [&](const auto &element)
                {
                    using T = std::decay_t<decltype(element)>;
                    if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, long long> || std::is_same_v<T, double> ||
                                  std::is_same_v<T, std::string>)
                        result.emplace_back(element);
                    else
                        throw std::runtime_error("ParamManager: nested or null YAML sequences are not supported.");
                },
                converted);
        }
        return result;
    }

    throw std::runtime_error("ParamManager: unsupported YAML value.");
}

ParamValue ParamManager::ConvertFromJson_(const json &value) const
{
    if (value.is_null()) return std::monostate{};
    if (value.is_boolean()) return value.get<bool>();
    if (value.is_number_integer()) return value.get<long long>();
    if (value.is_number_float()) return value.get<double>();
    if (value.is_string()) return value.get<std::string>();
    if (value.is_array())
    {
        MixedVector result;
        result.reserve(value.size());
        for (const auto &item : value)
        {
            if (item.is_boolean())
                result.emplace_back(item.get<bool>());
            else if (item.is_number_integer())
                result.emplace_back(item.get<long long>());
            else if (item.is_number_float())
                result.emplace_back(item.get<double>());
            else if (item.is_string())
                result.emplace_back(item.get<std::string>());
            else
                throw std::runtime_error("ParamManager: nested JSON values are not supported.");
        }
        return result;
    }
    throw std::runtime_error("ParamManager: object values are not supported.");
}

ParamValue ParamManager::CoerceToRegisteredType_(const std::string &key, const ParamValue &value) const
{
    const auto current = m_RawValues.find(key);
    if (current == m_RawValues.end()) throw std::runtime_error("ParamManager: key '" + key + "' is not registered.");

    return std::visit(
        [&](const auto &registered) -> ParamValue
        {
            using Target = std::decay_t<decltype(registered)>;
            if constexpr (std::is_same_v<Target, std::monostate>)
            {
                if (!std::holds_alternative<std::monostate>(value))
                    throw std::runtime_error("ParamManager: parameter '" + key + "' is registered as none.");
                return std::monostate{};
            }
            else if constexpr (std::is_same_v<Target, bool>)
            {
                if (const auto *typed = std::get_if<bool>(&value)) return *typed;
            }
            else if constexpr (std::is_same_v<Target, int> || std::is_same_v<Target, long> || std::is_same_v<Target, long long>)
            {
                if (const auto *typed = std::get_if<int>(&value)) return CheckedIntegralCast<Target>(*typed, key);
                if (const auto *typed = std::get_if<long>(&value)) return CheckedIntegralCast<Target>(*typed, key);
                if (const auto *typed = std::get_if<long long>(&value)) return CheckedIntegralCast<Target>(*typed, key);
                if (const auto *typed = std::get_if<double>(&value)) return CheckedIntegralCast<Target>(*typed, key);
            }
            else if constexpr (std::is_same_v<Target, double>)
            {
                if (const auto *typed = std::get_if<int>(&value)) return static_cast<double>(*typed);
                if (const auto *typed = std::get_if<long>(&value)) return static_cast<double>(*typed);
                if (const auto *typed = std::get_if<long long>(&value)) return static_cast<double>(*typed);
                if (const auto *typed = std::get_if<double>(&value)) return *typed;
            }
            else if constexpr (std::is_same_v<Target, std::string>)
            {
                if (const auto *typed = std::get_if<std::string>(&value)) return *typed;
            }
            else if constexpr (std::is_same_v<Target, std::vector<int>>)
            {
                if (const auto *typed = std::get_if<std::vector<int>>(&value)) return *typed;
                if (const auto *mixed = std::get_if<MixedVector>(&value))
                {
                    std::vector<int> result;
                    result.reserve(mixed->size());
                    for (const auto &item : *mixed)
                    {
                        if (const auto *integer = std::get_if<long long>(&item))
                            result.push_back(CheckedIntegralCast<int>(*integer, key));
                        else if (const auto *real = std::get_if<double>(&item))
                            result.push_back(CheckedIntegralCast<int>(*real, key));
                        else
                            throw std::runtime_error("ParamManager: non-integer list element for parameter '" + key + "'.");
                    }
                    return result;
                }
            }
            else if constexpr (std::is_same_v<Target, std::vector<double>>)
            {
                if (const auto *typed = std::get_if<std::vector<double>>(&value)) return *typed;
                if (const auto *integers = std::get_if<std::vector<int>>(&value))
                    return std::vector<double>(integers->begin(), integers->end());
                if (const auto *mixed = std::get_if<MixedVector>(&value))
                {
                    std::vector<double> result;
                    result.reserve(mixed->size());
                    for (const auto &item : *mixed)
                        result.push_back(MixedAsDouble(item, key));
                    return result;
                }
            }
            else if constexpr (std::is_same_v<Target, std::vector<std::string>>)
            {
                if (const auto *typed = std::get_if<std::vector<std::string>>(&value)) return *typed;
                if (const auto *mixed = std::get_if<MixedVector>(&value))
                {
                    std::vector<std::string> result;
                    result.reserve(mixed->size());
                    for (const auto &item : *mixed)
                    {
                        const auto *text = std::get_if<std::string>(&item);
                        if (!text) throw std::runtime_error("ParamManager: non-string list element for parameter '" + key + "'.");
                        result.push_back(*text);
                    }
                    return result;
                }
            }
            else if constexpr (std::is_same_v<Target, MixedVector>)
            {
                if (const auto *typed = std::get_if<MixedVector>(&value)) return *typed;
                if (const auto *integers = std::get_if<std::vector<int>>(&value))
                {
                    MixedVector result;
                    for (const int item : *integers)
                        result.emplace_back(static_cast<long long>(item));
                    return result;
                }
                if (const auto *reals = std::get_if<std::vector<double>>(&value))
                {
                    MixedVector result;
                    for (const double item : *reals)
                        result.emplace_back(item);
                    return result;
                }
                if (const auto *strings = std::get_if<std::vector<std::string>>(&value))
                {
                    MixedVector result;
                    for (const auto &item : *strings)
                        result.emplace_back(item);
                    return result;
                }
            }

            throw std::runtime_error("ParamManager: cannot assign " +
                                     std::visit([](const auto &item) { return ::TypeName<std::decay_t<decltype(item)>>(); }, value) +
                                     " to parameter '" + key + "' registered as " + ::TypeName<Target>() + ".");
        },
        current->second);
}

json ParamManager::ToJsonInternal_() const
{
    json document;
    for (const auto &[key, value] : m_RawValues)
    {
        json entry;
        entry["description"] = m_Descriptions.count(key) ? m_Descriptions.at(key) : "";
        std::visit(
            [&](const auto &typed)
            {
                using T = std::decay_t<decltype(typed)>;
                entry["type"] = TypeName<T>();
                if constexpr (std::is_same_v<T, std::monostate>)
                    entry["value"] = nullptr;
                else if constexpr (std::is_same_v<T, MixedVector>)
                {
                    entry["value"] = json::array();
                    for (const auto &item : typed)
                        entry["value"].push_back(MixedToJSON(item));
                }
                else
                    entry["value"] = typed;
            },
            value);
        document[key] = std::move(entry);
    }
    return document;
}

YAML::Node ParamManager::ToYamlInternal_() const
{
    YAML::Node document;
    for (const auto &[key, value] : m_RawValues)
    {
        YAML::Node entry;
        std::visit(
            [&](const auto &typed)
            {
                using T = std::decay_t<decltype(typed)>;
                entry["type"] = TypeName<T>();
                if constexpr (std::is_same_v<T, std::monostate>)
                    entry["value"] = YAML::Node();
                else if constexpr (std::is_same_v<T, MixedVector>)
                {
                    YAML::Node sequence(YAML::NodeType::Sequence);
                    for (const auto &item : typed)
                        sequence.push_back(MixedToYAML(item));
                    entry["value"] = sequence;
                }
                else
                    entry["value"] = typed;
            },
            value);
        entry["description"] = m_Descriptions.count(key) ? m_Descriptions.at(key) : "";
        document[key] = entry;
    }
    return document;
}

void ParamManager::LoadYAMLFile(const std::string &path) { SetParamsFromYAML(YAML::LoadFile(path)); }

void ParamManager::SaveYAMLFile(const std::string &path) const
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    std::ofstream output(path);
    if (!output) throw std::runtime_error("ParamManager: cannot open YAML file: " + path);
    output << DumpYAML(4);
    if (!output) throw std::runtime_error("ParamManager: failed to write YAML file: " + path);
}

void ParamManager::LoadJSONFile(const std::string &path)
{
    std::ifstream input(path);
    if (!input) throw std::runtime_error("ParamManager: cannot open JSON file: " + path);
    std::stringstream buffer;
    buffer << input.rdbuf();
    SetParamsFromJSON(buffer.str());
}

void ParamManager::SetParamsFromJSON(const std::string &serialized)
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    const json document = json::parse(serialized);
    if (!document.is_object()) throw std::runtime_error("ParamManager: JSON root must be an object.");

    for (const auto &[key, entry] : document.items())
    {
        if (!Has(key)) throw std::runtime_error("ParamManager: key '" + key + "' is not registered.");
        if (entry.is_object() && entry.contains("type") && entry.at("type").get<std::string>() != TypeOf(key))
            throw std::runtime_error("ParamManager: serialized type for '" + key + "' does not match its registered type.");
        const json &value = entry.is_object() && entry.contains("value") ? entry.at("value") : entry;
        const std::string description =
            entry.is_object() && entry.contains("description") ? entry.at("description").get<std::string>() : "";
        UpdateExisting_(key, ConvertFromJson_(value), description);
        if (entry.is_object() && entry.contains("description")) m_Descriptions[key] = description;
    }
}

void ParamManager::SaveJSONFile(const std::string &path) const
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    std::ofstream output(path);
    if (!output) throw std::runtime_error("ParamManager: cannot open JSON file: " + path);
    output << DumpJSON(2);
    if (!output) throw std::runtime_error("ParamManager: failed to write JSON file: " + path);
}

std::string ParamManager::DumpYAML(int indent) const
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    YAML::Emitter output;
    output.SetIndent(indent);
    output.SetMapFormat(YAML::Block);
    output.SetSeqFormat(YAML::Flow);
    output << ToYamlInternal_();
    if (!output.good()) throw std::runtime_error("ParamManager: failed to serialize YAML.");
    return output.c_str();
}

std::string ParamManager::DumpJSON(int indent) const
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    return ToJsonInternal_().dump(indent);
}

void ParamManager::SetParamsFromYAML(const YAML::Node &document)
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    if (!document.IsMap()) throw std::runtime_error("ParamManager: YAML root must be a map.");
    for (const auto &item : document)
    {
        const std::string key = item.first.as<std::string>();
        if (!Has(key)) throw std::runtime_error("ParamManager: key '" + key + "' is not registered.");
        const YAML::Node entry = item.second;
        if (entry.IsMap() && entry["type"] && entry["type"].as<std::string>() != TypeOf(key))
            throw std::runtime_error("ParamManager: serialized type for '" + key + "' does not match its registered type.");
        const YAML::Node value = entry.IsMap() && entry["value"] ? entry["value"] : entry;
        const std::string description = entry.IsMap() && entry["description"] ? entry["description"].as<std::string>() : "";
        UpdateExisting_(key, ConvertFromYaml_(value), description);
        if (entry.IsMap() && entry["description"]) m_Descriptions[key] = description;
    }
}

YAML::Node ParamManager::ToYAMLNode() const
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    return ToYamlInternal_();
}

void ParamManager::RegisterCommon()
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    if (!Has("dry_run")) Register("dry_run", false, "simulate execution only");
    if (!Has("force_run")) Register("force_run", false, "force execution");
}
