#include "ParamManager.hh"

#include <cmath>
#include <fstream>
#include <limits>

using json = nlohmann::json;

ParamManager::ParamManager() { RegisterCommon(); }

// ================= YAML -> ParamValue =================
ParamValue ParamManager::ConvertFromYAML(const YAML::Node &val)
{
    if (val.IsScalar())
    {
        const std::string s = val.Scalar();
        if (s == "true") return true;
        if (s == "false") return false;
        try
        {
            double d = std::stod(s);
            if (std::floor(d) == d) return static_cast<long long>(d);
            return d;
        }
        catch (...)
        {
            return s;
        }
    }

    if (val.IsSequence())
    {
        if (val.size() == 0) return std::monostate{};

        bool all_int = true, all_double = true, all_str = true;
        for (const auto &e : val)
        {
            if (!e.IsScalar())
            {
                all_int = all_double = all_str = false;
                break;
            }
            const std::string s = e.Scalar();
            try
            {
                std::stoll(s);
            }
            catch (...)
            {
                all_int = false;
            }
            try
            {
                std::stod(s);
            }
            catch (...)
            {
                all_double = false;
            }
        }

        if (all_int) return val.as<std::vector<int>>();
        if (all_double) return val.as<std::vector<double>>();
        if (all_str) return val.as<std::vector<std::string>>();

        // heterogeneous → MixedVector
        MixedVector mv;
        mv.reserve(val.size());
        for (const auto &e : val)
        {
            const std::string s = e.Scalar();
            if (s == "true")
            {
                mv.emplace_back(true);
                continue;
            }
            if (s == "false")
            {
                mv.emplace_back(false);
                continue;
            }
            try
            {
                mv.emplace_back(std::stoll(s));
                continue;
            }
            catch (...)
            {
            }
            try
            {
                mv.emplace_back(std::stod(s));
                continue;
            }
            catch (...)
            {
            }
            mv.emplace_back(s);
        }
        return mv;
    }

    if (val.IsMap())
    {
        json j;
        for (const auto &kv : val)
            j[kv.first.as<std::string>()] = kv.second.as<std::string>();
        return j.dump();
    }

    return std::monostate{};
}

// ================= PY -> ParamValue =================
ParamValue ParamManager::ConvertFromPy(const py::object &obj) const
{
    if (py::isinstance<py::bool_>(obj)) return obj.cast<bool>();
    if (py::isinstance<py::int_>(obj)) return static_cast<long long>(obj.cast<long long>());
    if (py::isinstance<py::float_>(obj)) return obj.cast<double>();
    if (py::isinstance<py::str>(obj)) return obj.cast<std::string>();

    if (py::isinstance<py::list>(obj))
    {
        py::list lst = obj.cast<py::list>();
        if (lst.empty()) return std::monostate{};

        bool all_str = true, all_int = true, all_float = true;
        for (auto item : lst)
        {
            all_str &= py::isinstance<py::str>(item);
            all_int &= py::isinstance<py::int_>(item);
            all_float &= py::isinstance<py::float_>(item);
            if (!(all_str || all_int || all_float)) break;
        }

        if (all_str) return lst.cast<std::vector<std::string>>();
        if (all_int) return lst.cast<std::vector<int>>();
        if (all_float) return lst.cast<std::vector<double>>();

        MixedVector mv;
        mv.reserve(py::len(lst));
        for (auto item : lst)
        {
            if (py::isinstance<py::bool_>(item))
                mv.emplace_back(item.cast<bool>());
            else if (py::isinstance<py::int_>(item))
                mv.emplace_back(static_cast<long long>(item.cast<long long>()));
            else if (py::isinstance<py::float_>(item))
                mv.emplace_back(item.cast<double>());
            else if (py::isinstance<py::str>(item))
                mv.emplace_back(item.cast<std::string>());
            else
                throw std::runtime_error("ParamManager: unsupported element type in Python list.");
        }
        return mv;
    }

    throw std::runtime_error("ParamManager: unsupported Python type.");
}

// ============== Internal ==============
static json MixedToJSON(const MixedElement &e)
{
    return std::visit([](auto &&v) -> json { return json(v); }, e);
}
static YAML::Node MixedToYAML(const MixedElement &e)
{
    return std::visit([](auto &&v) -> YAML::Node { return YAML::Node(v); }, e);
}

json ParamManager::ToJSONInternal() const
{
    json j;
    for (const auto &[k, v] : rawValues_)
    {
        json entry;
        entry["description"] = descriptions_.count(k) ? descriptions_.at(k) : "";
        std::visit(
            [&](auto &&val)
            {
                using T = std::decay_t<decltype(val)>;

                if constexpr (std::is_same_v<T, std::monostate>)
                {
                    entry["value"] = nullptr; // monostate → null
                }
                else if constexpr (std::is_same_v<T, MixedVector>)
                {
                    json arr = json::array();
                    for (const auto &e : val)
                        arr.push_back(MixedToJSON(e));
                    entry["value"] = std::move(arr);
                }
                else
                {
                    entry["value"] = val;
                }

                entry["type"] = TypeName<T>();
            },
            v);

        j[k] = std::move(entry);
    }
    return j;
}

YAML::Node ParamManager::ToYAMLInternal() const
{
    YAML::Node node;
    for (const auto &[k, v] : rawValues_)
    {
        YAML::Node entry;
        std::visit(
            [&](auto &&val)
            {
                using T = std::decay_t<decltype(val)>;

                if constexpr (std::is_same_v<T, std::monostate>)
                {
                    entry["value"] = YAML::Node(); // monostate → Null Node
                }
                else if constexpr (std::is_same_v<T, MixedVector>)
                {
                    YAML::Node seq(YAML::NodeType::Sequence);
                    for (const auto &e : val)
                        seq.push_back(MixedToYAML(e));
                    entry["value"] = seq;
                }
                else
                {
                    entry["value"] = val;
                }

                entry["type"] = TypeName<T>();
            },
            v);

        if (descriptions_.count(k)) entry["desc"] = descriptions_.at(k);

        node[k] = entry;
    }
    return node;
}

// ============== Public I/O ==============
void ParamManager::LoadYAMLFile(const std::string &path)
{
    YAML::Node node = YAML::LoadFile(path);
    SetParamsFromYAML(node);
}

void ParamManager::SaveYAMLFile(const std::string &path) const
{
    YAML::Emitter out;
    out.SetIndent(4);
    out.SetMapFormat(YAML::Block);
    out.SetSeqFormat(YAML::Flow);
    out << ToYAMLInternal();

    std::ofstream fout(path);
    if (!fout.is_open()) throw std::runtime_error("ParamManager: cannot open YAML file: " + path);
    fout << out.c_str();
}

void ParamManager::LoadJSONFile(const std::string &path)
{
    std::ifstream fin(path);
    if (!fin.is_open()) throw std::runtime_error("ParamManager: cannot open JSON file: " + path);
    json j;
    fin >> j;

    for (auto &[k, entry] : j.items())
    {
        if (!Has(k)) throw std::runtime_error("ParamManager: key '" + k + "' not registered (LoadJSONFile).");

        if (!entry.contains("value")) continue;

        // value
        if (entry["value"].is_boolean())
            UpdateExisting(k, entry["value"].get<bool>());
        else if (entry["value"].is_number_integer())
            UpdateExisting(k, static_cast<long long>(entry["value"].get<long long>()));
        else if (entry["value"].is_number_float())
            UpdateExisting(k, entry["value"].get<double>());
        else if (entry["value"].is_string())
            UpdateExisting(k, entry["value"].get<std::string>());
        else if (entry["value"].is_array())
        {
            auto arr = entry["value"];
            bool all_int = true, all_double = true, all_str = true;
            for (auto &a : arr)
            {
                all_int &= a.is_number_integer();
                all_double &= a.is_number_float();
                all_str &= a.is_string();
            }
            if (all_int)
                UpdateExisting(k, arr.get<std::vector<int>>());
            else if (all_double)
                UpdateExisting(k, arr.get<std::vector<double>>());
            else if (all_str)
                UpdateExisting(k, arr.get<std::vector<std::string>>());
            else
            {
                MixedVector mv;
                mv.reserve(arr.size());
                for (auto &a : arr)
                {
                    if (a.is_boolean())
                        mv.emplace_back(a.get<bool>());
                    else if (a.is_number_integer())
                        mv.emplace_back(static_cast<long long>(a.get<long long>()));
                    else if (a.is_number_float())
                        mv.emplace_back(a.get<double>());
                    else if (a.is_string())
                        mv.emplace_back(a.get<std::string>());
                }
                UpdateExisting(k, mv);
            }
        }
        else
        {
            throw std::runtime_error("ParamManager: unsupported JSON value type for key: " + k);
        }
    }
}

void ParamManager::SaveJSONFile(const std::string &path) const
{
    std::ofstream fout(path);
    if (!fout.is_open()) throw std::runtime_error("ParamManager: cannot open JSON file: " + path);
    fout << ToJSONInternal().dump(2);
}

std::string ParamManager::DumpYAML(int indent) const
{
    YAML::Emitter out;
    out.SetIndent(indent);
    out.SetMapFormat(YAML::Block);
    out.SetSeqFormat(YAML::Flow);
    out << ToYAMLInternal();
    return out.c_str();
}

std::string ParamManager::DumpJSON(int indent) const { return ToJSONInternal().dump(indent); }

// ============== Bulk setters ==============
void ParamManager::SetParamsFromYAML(const YAML::Node &node)
{
    for (const auto &it : node)
    {
        const std::string key = it.first.as<std::string>();
        if (!Has(key)) throw std::runtime_error("ParamManager: key '" + key + "' not registered (SetParamsFromYAML).");

        ParamValue v = ConvertFromYAML(it.second);
        std::visit([&](auto &&x) { UpdateExisting(key, x); }, v);
    }
}

void ParamManager::SetParamsFromDict(const py::dict &d)
{
    for (auto item : d)
    {
        const std::string key = py::str(item.first);
        if (!Has(key)) throw std::runtime_error("ParamManager: key '" + key + "' not registered (SetParamsFromDict).");

        py::object val = py::reinterpret_borrow<py::object>(item.second);
        SetParamFromPy(key, val);
    }
}

void ParamManager::SetParamFromPy(const std::string &key, const py::object &obj, const std::string &desc)
{
    if (!Has(key)) throw std::runtime_error("ParamManager: key '" + key + "' not registered (SetParamFromPy).");

    ParamValue v = ConvertFromPy(obj);
    std::visit([&](auto &&val) { UpdateExisting(key, val, desc); }, v);
}

// ======== AMCM compatibility ========
YAML::Node ParamManager::ToYAMLNode() const { return ToYAMLInternal(); }

// ============== Common ==============
void ParamManager::RegisterCommon()
{
    Register("dry_run", false, "simulate execution only");
    Register("force_run", false, "force execution");
}
