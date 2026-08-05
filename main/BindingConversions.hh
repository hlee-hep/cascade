#pragma once

#include "ModuleRun.hh"
#include "ParamManager.hh"
#include "PluginTrust.hh"
#include <filesystem>
#include <limits>
#include <optional>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;

namespace cascade::python_binding
{
inline ParamValue ParamValueFromPython(const py::handle &object)
{
    if (object.is_none()) return std::monostate{};
    if (py::isinstance<py::bool_>(object)) return py::cast<bool>(object);
    if (py::isinstance<py::int_>(object)) return py::cast<long long>(object);
    if (py::isinstance<py::float_>(object)) return py::cast<double>(object);
    if (py::isinstance<py::str>(object)) return py::cast<std::string>(object);
    if (py::isinstance<py::list>(object) || py::isinstance<py::tuple>(object))
    {
        MixedVector result;
        for (const auto &item : py::reinterpret_borrow<py::iterable>(object))
        {
            if (py::isinstance<py::bool_>(item))
                result.emplace_back(py::cast<bool>(item));
            else if (py::isinstance<py::int_>(item))
                result.emplace_back(py::cast<long long>(item));
            else if (py::isinstance<py::float_>(item))
                result.emplace_back(py::cast<double>(item));
            else if (py::isinstance<py::str>(item))
                result.emplace_back(py::cast<std::string>(item));
            else
                throw py::type_error("ParamManager: unsupported element in Python sequence.");
        }
        return result;
    }
    throw py::type_error("ParamManager: unsupported Python value.");
}

inline ParamValue RegisteredParamValueFromPython(const py::handle &object)
{
    if (!py::isinstance<py::list>(object) && !py::isinstance<py::tuple>(object))
        return ParamValueFromPython(object);

    const py::sequence values = py::reinterpret_borrow<py::sequence>(object);
    if (values.empty()) return MixedVector{};

    bool allIntegers = true;
    bool allNumeric = true;
    bool allStrings = true;
    for (const py::handle item : values)
    {
        const bool integer = py::isinstance<py::int_>(item) && !py::isinstance<py::bool_>(item);
        const bool numeric = integer || py::isinstance<py::float_>(item);
        allIntegers = allIntegers && integer;
        allNumeric = allNumeric && numeric;
        allStrings = allStrings && py::isinstance<py::str>(item);
    }

    if (allIntegers)
    {
        std::vector<int> result;
        result.reserve(values.size());
        for (const py::handle item : values)
        {
            const long long value = py::cast<long long>(item);
            if (value < std::numeric_limits<int>::lowest() || value > std::numeric_limits<int>::max())
                return ParamValueFromPython(object);
            result.push_back(static_cast<int>(value));
        }
        return result;
    }
    if (allNumeric)
    {
        std::vector<double> result;
        result.reserve(values.size());
        for (const py::handle item : values) result.push_back(py::cast<double>(item));
        return result;
    }
    if (allStrings) return py::cast<std::vector<std::string>>(object);
    return ParamValueFromPython(object);
}

inline py::object ParamValueToPython(const ParamValue &value)
{
    return std::visit(
        [](const auto &item) -> py::object
        {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, std::monostate>)
                return py::none();
            else if constexpr (std::is_same_v<T, MixedVector>)
            {
                py::list result;
                for (const auto &element : item)
                    std::visit([&](const auto &typed) { result.append(py::cast(typed)); }, element);
                return result;
            }
            else
                return py::cast(item);
        },
        value);
}

inline py::dict ParamManagerToPython(const ParamManager &parameters)
{
    py::dict result;
    for (const auto &[key, value] : parameters.RawValues()) result[py::str(key)] = ParamValueToPython(value);
    return result;
}

inline py::object PythonPath(const std::filesystem::path &path)
{
    return py::module_::import("pathlib").attr("Path")(path.string());
}

inline std::string PythonPathText(const py::object &path) { return py::str(path).cast<std::string>(); }

inline ModuleStatus StatusFromText(const std::string &text)
{
    for (const auto value : {ModuleStatus::Pending, ModuleStatus::Initializing, ModuleStatus::Running,
                             ModuleStatus::Finalizing, ModuleStatus::Done, ModuleStatus::Skipped,
                             ModuleStatus::Interrupted, ModuleStatus::Failed})
        if (text == ToString(value)) return value;
    throw std::invalid_argument("Unknown module status: " + text);
}

inline ModulePhase PhaseFromText(const std::string &text)
{
    for (const auto value : {ModulePhase::None, ModulePhase::Init, ModulePhase::Check, ModulePhase::Execute,
                             ModulePhase::Finalize, ModulePhase::Commit})
        if (text == ToString(value)) return value;
    throw std::invalid_argument("Unknown module phase: " + text);
}

inline std::optional<PluginOrigin> PluginOriginFromPython(const py::object &value)
{
    if (value.is_none()) return std::nullopt;
    const py::dict source = py::cast<py::dict>(value);
    PluginOrigin origin;
    origin.Package = py::cast<std::string>(source["package"]);
    origin.ManifestPath = py::cast<std::string>(source["manifest_path"]);
    origin.ManifestSha256 = py::cast<std::string>(source["manifest_sha256"]);
    origin.ArtifactSha256 = py::cast<std::string>(source["artifact_sha256"]);
    if (source.contains("signer_fingerprint") && !source["signer_fingerprint"].is_none())
        origin.SignerFingerprint = py::cast<std::string>(source["signer_fingerprint"]);
    origin.Trust = source.contains("trust") && py::cast<std::string>(source["trust"]) == "Signed"
                       ? PluginTrustStatus::Signed
                       : PluginTrustStatus::Verified;
    return origin;
}
} // namespace cascade::python_binding
