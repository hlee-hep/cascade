#include "AMCM.hh"
#include "AnalysisManager.hh"
#include "AnalysisModuleRegistry.hh"
#include "CacheManager.hh"
#include "DAGManager.hh"
#include "ExecutionContext.hh"
#include "IAnalysisModule.hh"
#include "InterruptManager.hh"
#include "Logger.hh"
#include "ParamManager.hh"
#include "PluginABI.hh"
#include "PluginPaths.hh"
#include "PluginVerifier.hh"
#include "Provenance.hh"
#include "SnapshotHasher.hh"
#include "Version.hh"
#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <limits>

namespace py = pybind11;

namespace
{
ParamValue ParamValueFromPython(const py::handle &object)
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

ParamValue RegisteredParamValueFromPython(const py::handle &object)
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

py::object ParamValueToPython(const ParamValue &value)
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

py::dict ParamManagerToPython(const ParamManager &parameters)
{
    py::dict result;
    for (const auto &[key, value] : parameters.RawValues()) result[py::str(key)] = ParamValueToPython(value);
    return result;
}

py::object PythonPath(const std::filesystem::path &path)
{
    return py::module_::import("pathlib").attr("Path")(path.string());
}

std::string PythonPathText(const py::object &path) { return py::str(path).cast<std::string>(); }

ModuleStatus StatusFromText(const std::string &text)
{
    for (const auto value : {ModuleStatus::Pending, ModuleStatus::Initializing, ModuleStatus::Running,
                             ModuleStatus::Finalizing, ModuleStatus::Done, ModuleStatus::Skipped,
                             ModuleStatus::Interrupted, ModuleStatus::Failed})
        if (text == ToString(value)) return value;
    throw std::invalid_argument("Unknown module status: " + text);
}

ModulePhase PhaseFromText(const std::string &text)
{
    for (const auto value : {ModulePhase::None, ModulePhase::Init, ModulePhase::Check, ModulePhase::Execute,
                             ModulePhase::Finalize, ModulePhase::Commit})
        if (text == ToString(value)) return value;
    throw std::invalid_argument("Unknown module phase: " + text);
}

class PythonAnalysisModule : public IAnalysisModule
{
  public:
    using IAnalysisModule::IAnalysisModule;

    void Description() const override { CallVoid_("print_description"); }

    ModuleMetadata GetMetadata() const override
    {
        py::gil_scoped_acquire acquire;
        try
        {
            py::function method = py::get_override(static_cast<const IAnalysisModule *>(this), "get_metadata");
            if (!method) return IAnalysisModule::GetMetadata();
            const py::dict value = py::cast<py::dict>(method());
            ModuleMetadata metadata;
            metadata.Name = py::cast<std::string>(value["name"]);
            metadata.Version = py::cast<std::string>(value["version"]);
            metadata.Summary = py::cast<std::string>(value["summary"]);
            metadata.Tags = py::cast<std::vector<std::string>>(value["tags"]);
            return metadata;
        }
        catch (const py::error_already_set &error)
        {
            throw std::runtime_error(error.what());
        }
    }

  protected:
    void Init() override { CallVoid_("init"); }
    void Execute() override { CallVoid_("execute"); }
    void Finalize() override { CallVoid_("finalize"); }
    void OnFailure(ModulePhase phase, const std::string &message) override { CallVoid_("on_failure", phase, message); }
    bool UsesAnalysisManagers() const override { return false; }
    std::string RuntimeLanguage() const override { return "python"; }

    std::string AnalysisSnapshotState() const override
    {
        py::gil_scoped_acquire acquire;
        try
        {
            py::function method = py::get_override(static_cast<const IAnalysisModule *>(this), "snapshot_state");
            if (!method) return "{}";
            py::module_ json = py::module_::import("json");
            return py::cast<std::string>(json.attr("dumps")(
                method(), py::arg("sort_keys") = true,
                py::arg("default") = py::module_::import("builtins").attr("str")));
        }
        catch (const py::error_already_set &error)
        {
            throw std::runtime_error(error.what());
        }
    }

  private:
    template <typename... Args> void CallVoid_(const char *name, Args &&...args) const
    {
        py::gil_scoped_acquire acquire;
        try
        {
            py::function method = py::get_override(static_cast<const IAnalysisModule *>(this), name);
            if (!method) throw std::runtime_error(std::string("Python module must implement ") + name + "()");
            method(std::forward<Args>(args)...);
        }
        catch (const py::error_already_set &error)
        {
            throw std::runtime_error(error.what());
        }
    }
};

std::optional<PluginOrigin> PluginOriginFromPython(const py::object &value)
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
} // namespace

PYBIND11_MODULE(_cascade, m)
{
    py::enum_<PluginTrustPolicy>(m, "PluginTrustPolicy")
        .value("Verified", PluginTrustPolicy::Verified)
        .value("RequireSigned", PluginTrustPolicy::RequireSigned);
    py::enum_<PluginTrustStatus>(m, "PluginTrustStatus")
        .value("Verified", PluginTrustStatus::Verified)
        .value("Signed", PluginTrustStatus::Signed);
    py::class_<PluginOrigin>(m, "PluginOrigin")
        .def_readonly("package", &PluginOrigin::Package)
        .def_readonly("manifest_path", &PluginOrigin::ManifestPath)
        .def_readonly("manifest_sha256", &PluginOrigin::ManifestSha256)
        .def_readonly("artifact_sha256", &PluginOrigin::ArtifactSha256)
        .def_readonly("signer_fingerprint", &PluginOrigin::SignerFingerprint)
        .def_readonly("trust", &PluginOrigin::Trust);
    py::class_<VerifiedPluginArtifact>(m, "VerifiedPluginArtifact")
        .def_readonly("name", &VerifiedPluginArtifact::Name)
        .def_readonly("language", &VerifiedPluginArtifact::Language)
        .def_readonly("path", &VerifiedPluginArtifact::Path)
        .def_readonly("sha256", &VerifiedPluginArtifact::Sha256)
        .def_readonly("classes", &VerifiedPluginArtifact::Classes)
        .def_property_readonly("source", [](const VerifiedPluginArtifact &artifact) { return py::bytes(artifact.Source); })
        .def_readonly("origin", &VerifiedPluginArtifact::Origin);
    py::class_<VerifiedPluginPackage>(m, "VerifiedPluginPackage")
        .def_readonly("package", &VerifiedPluginPackage::Package)
        .def_readonly("manifest_path", &VerifiedPluginPackage::ManifestPath)
        .def_readonly("manifest_sha256", &VerifiedPluginPackage::ManifestSha256)
        .def_readonly("trusted_key_path", &VerifiedPluginPackage::TrustedKeyPath)
        .def_readonly("signer_fingerprint", &VerifiedPluginPackage::SignerFingerprint)
        .def_readonly("trust", &VerifiedPluginPackage::Trust)
        .def_readonly("artifacts", &VerifiedPluginPackage::Artifacts);
    py::class_<PluginDiscoveryResult>(m, "PluginDiscoveryResult")
        .def_readonly("packages", &PluginDiscoveryResult::Packages)
        .def_readonly("errors", &PluginDiscoveryResult::Errors);
    py::class_<PluginVerifier>(m, "PluginVerifier")
        .def_static("index_fingerprint",
                    [](const std::vector<std::string> &pluginRoots, const std::vector<std::string> &trustStores)
                    {
                        py::gil_scoped_release release;
                        return PluginVerifier::IndexFingerprint(pluginRoots, trustStores);
                    },
                    py::arg("plugin_roots"), py::arg("trust_stores"))
        .def_static("discover",
                    [](const std::vector<std::string> &pluginRoots, PluginTrustPolicy policy,
                       const std::string &language)
                    {
                        py::gil_scoped_release release;
                        return PluginVerifier::Discover(pluginRoots, policy, language);
                    },
                    py::arg("plugin_roots"), py::arg("policy") = PluginTrustPolicy::Verified,
                    py::arg("language") = "")
        .def_static("hash_file", [](const std::string &path)
                    { py::gil_scoped_release release; return PluginVerifier::HashFile(path); })
        .def_static("hash_bytes", [](const py::bytes &data)
                    { return PluginVerifier::HashBytes(py::cast<std::string>(data)); })
        .def_static("read_file", [](const std::string &path)
                    {
                        std::string data;
                        {
                            py::gil_scoped_release release;
                            data = PluginVerifier::ReadFile(path);
                        }
                        return py::bytes(data);
                    })
        .def_static("validate_directory_tree", [](const std::string &path, bool allowMissingLeaf)
                    { py::gil_scoped_release release; PluginVerifier::ValidateDirectoryTree(path, allowMissingLeaf); },
                    py::arg("path"), py::arg("allow_missing_leaf") = false)
        .def_static("validate_staged_tree", [](const std::string &path)
                    { py::gil_scoped_release release; PluginVerifier::ValidateStagedTree(path); })
        .def_static("verify_package",
                    [](const std::string &packageDirectory, const std::string &trustStore, PluginTrustPolicy policy,
                       const std::string &language, const std::string &trustedKey,
                       const std::string &moduleIdentity)
                    {
                        py::gil_scoped_release release;
                        return PluginVerifier::VerifyPackage(packageDirectory, trustStore, policy, language, trustedKey,
                                                             moduleIdentity);
                    },
                    py::arg("package_directory"), py::arg("trust_store"),
                    py::arg("policy") = PluginTrustPolicy::Verified, py::arg("language") = "",
                    py::arg("trusted_key") = "", py::arg("module_identity") = "");
    py::class_<PluginLayout>(m, "PluginLayout")
        .def_readonly("prefix", &PluginLayout::Prefix)
        .def_readonly("cpp", &PluginLayout::Cpp)
        .def_readonly("python", &PluginLayout::Python)
        .def_readonly("include", &PluginLayout::Include)
        .def_readonly("trust_store", &PluginLayout::TrustStore)
        .def_readonly("source", &PluginLayout::Source);
    py::class_<PluginPaths>(m, "PluginPaths")
        .def_static("config_path", &PluginPaths::ConfigPath)
        .def_static("runtime_prefix", &PluginPaths::RuntimePrefix)
        .def_static("canonical_prefix", &PluginPaths::CanonicalPrefix)
        .def_static("unique", &PluginPaths::Unique)
        .def_static("load_config",
                    [](const std::string &path)
                    {
                        const auto config = PluginPaths::LoadConfig(path);
                        py::list entries;
                        for (const auto &entry : config.Prefixes)
                        {
                            py::dict item;
                            item["path"] = entry.Path;
                            item["enabled"] = entry.Enabled;
                            entries.append(std::move(item));
                        }
                        py::dict result;
                        result["schema"] = config.Schema;
                        result["plugin_prefixes"] = std::move(entries);
                        return result;
                    },
                    py::arg("path") = "")
        .def_static("configured_prefixes", &PluginPaths::ConfiguredPrefixes, py::arg("path") = "")
        .def_static("add_prefix", &PluginPaths::AddPrefix, py::arg("prefix"), py::arg("path") = "")
        .def_static("remove_prefix", &PluginPaths::RemovePrefix, py::arg("prefix"), py::arg("path") = "")
        .def_static("layout", &PluginPaths::Layout)
        .def_static("runtime_layouts", &PluginPaths::RuntimeLayouts)
        .def_static("roots", &PluginPaths::Roots)
        .def_static("trust_store_for_root", &PluginPaths::TrustStoreForRoot);
    py::class_<ParamManager>(m, "ParamManager")
        .def(py::init<>())
        .def("register",
             [](ParamManager &parameters, const std::string &key, const py::object &value,
                const std::string &description)
             {
                 if (parameters.Has(key)) throw py::key_error("Parameter already registered: " + key);
                 parameters.Register<ParamValue>(key, RegisteredParamValueFromPython(value), description);
             },
             py::arg("name"), py::arg("default"), py::arg("description") = "")
        .def("set_registered",
             [](ParamManager &parameters, const std::string &key, const py::object &value)
             {
                 if (!parameters.Has(key)) throw py::key_error("Parameter is not registered: " + key);
                 try
                 {
                     parameters.SetParamVariant(key, ParamValueFromPython(value));
                 }
                 catch (const py::error_already_set &)
                 {
                     throw;
                 }
                 catch (const std::exception &error)
                 {
                     throw py::type_error(error.what());
                 }
             })
        .def("get",
             [](const ParamManager &parameters, const std::string &key, const py::object &fallback)
             { return parameters.Has(key) ? ParamValueToPython(parameters.Get<ParamValue>(key)) : fallback; },
             py::arg("name"), py::arg("default") = py::none())
        .def("type_of", &ParamManager::TypeOf)
        .def("to_dict", &ParamManagerToPython)
        .def("load_yaml_file",
             [](ParamManager &parameters, const std::string &path)
             {
                 py::gil_scoped_release release;
                 parameters.LoadYAMLFile(path);
             })
        .def("save_yaml_file",
             [](const ParamManager &parameters, const std::string &path)
             {
                 py::gil_scoped_release release;
                 parameters.SaveYAMLFile(path);
             })
        .def("load_json_file",
             [](ParamManager &parameters, const std::string &path)
             {
                 py::gil_scoped_release release;
                 parameters.LoadJSONFile(path);
             })
        .def("set_params_from_json",
             [](ParamManager &parameters, const std::string &document)
             {
                 py::gil_scoped_release release;
                 parameters.SetParamsFromJSON(document);
             })
        .def("save_json_file",
             [](const ParamManager &parameters, const std::string &path)
             {
                 py::gil_scoped_release release;
                 parameters.SaveJSONFile(path);
             })
        .def("dump_yaml", &ParamManager::DumpYAML, py::arg("indent") = 2)
        .def("dump_json", &ParamManager::DumpJSON, py::arg("indent") = 2)
        .def("keys",
             [](const ParamManager &parameters)
             {
                 std::vector<std::string> keys;
                 keys.reserve(parameters.RawValues().size());
                 for (const auto &[key, unused] : parameters.RawValues()) keys.push_back(key);
                 return keys;
             })
        .def("__iter__",
             [](const ParamManager &parameters)
             {
                 py::list keys;
                 for (const auto &[key, unused] : parameters.RawValues()) keys.append(key);
                 return keys.attr("__iter__")();
             })
        .def("__len__", [](const ParamManager &parameters) { return parameters.RawValues().size(); })
        .def("__contains__", &ParamManager::Has)
        .def("__getitem__",
             [](const ParamManager &parameters, const std::string &key)
             {
                 if (!parameters.Has(key)) throw py::key_error(key);
                 return ParamValueToPython(parameters.Get<ParamValue>(key));
             })
        .def("__setitem__",
             [](ParamManager &parameters, const std::string &key, const py::object &value)
             {
                 if (!parameters.Has(key))
                 {
                     parameters.Register<ParamValue>(key, RegisteredParamValueFromPython(value));
                     return;
                 }
                 try
                 {
                     parameters.SetParamVariant(key, ParamValueFromPython(value));
                 }
                 catch (const py::error_already_set &)
                 {
                     throw;
                 }
                 catch (const std::exception &error)
                 {
                     throw py::type_error(error.what());
                 }
             });
    py::class_<SnapshotHasher>(m, "SnapshotHasher")
        .def_static("compute",
                    [](const ParamManager &parameters, const std::string &moduleName,
                       const std::string &codeVersion, const std::string &analysisState,
                       const std::string &executionState, const std::string &pluginArtifactHash)
                    {
                        py::gil_scoped_release release;
                        return SnapshotHasher::ComputeSerialized(parameters, moduleName, codeVersion,
                                                                 analysisState, executionState, pluginArtifactHash);
                    },
                    py::arg("parameters"), py::arg("module_name"), py::arg("code_version"),
                    py::arg("analysis_state"), py::arg("execution_state") = "",
                    py::arg("plugin_artifact_hash") = "");
    py::class_<CacheSnapshot>(m, "CacheSnapshot")
        .def_readonly("module", &CacheSnapshot::Module)
        .def_readonly("hash", &CacheSnapshot::Hash)
        .def_readonly("provenance", &CacheSnapshot::Provenance)
        .def_readonly("cache_file", &CacheSnapshot::CacheFile);
    py::class_<CacheManager>(m, "CacheManager")
        .def_static("cache_dir", &CacheManager::CacheDir)
        .def_static("is_hash_cached",
                    [](const std::string &module, const std::string &hash, const std::string &directory)
                    {
                        py::gil_scoped_release release;
                        return CacheManager::IsHashCached(module, hash, directory);
                    })
        .def_static("find_provenance",
                    [](const std::string &module, const std::string &hash, const std::string &directory)
                    {
                        py::gil_scoped_release release;
                        return CacheManager::FindProvenance(module, hash, directory);
                    })
        .def_static("add_hash",
                    [](const std::string &module, const std::string &hash, const std::string &directory,
                       const std::string &provenance)
                    {
                        py::gil_scoped_release release;
                        CacheManager::AddHash(module, hash, directory, provenance);
                    },
                    py::arg("module"), py::arg("hash"), py::arg("directory"), py::arg("provenance") = "")
        .def_static("remove_hash",
                    [](const std::string &module, const std::string &hash, const std::string &directory)
                    {
                        py::gil_scoped_release release;
                        CacheManager::RemoveHash(module, hash, directory);
                    })
        .def_static("list_snapshots",
                    [](const std::string &directory, const std::string &module)
                    {
                        py::gil_scoped_release release;
                        return CacheManager::ListSnapshots(directory, module);
                    },
                    py::arg("directory"), py::arg("module") = "")
        .def_static("prune",
                    [](const std::string &directory, const std::string &module, bool removeAll, bool dryRun)
                    {
                        py::gil_scoped_release release;
                        return CacheManager::Prune(directory, module, removeAll, dryRun);
                    },
                    py::arg("directory"), py::arg("module") = "", py::arg("remove_all") = false,
                    py::arg("dry_run") = false);
    py::class_<CancellationToken>(m, "CancellationToken")
        .def(py::init<>())
        .def("request", &CancellationToken::Request)
        .def("reset", &CancellationToken::Reset)
        .def("is_cancellation_requested", &CancellationToken::IsCancellationRequested);
    py::class_<OutputTransaction>(m, "OutputTransaction")
        .def(py::init<>())
        .def("begin", [](OutputTransaction &self, const py::object &root, const std::string &runId)
             { self.Begin(PythonPathText(root), runId); })
        .def("stage", [](OutputTransaction &self, const py::object &path)
             { return PythonPath(self.Stage(PythonPathText(path))); })
        .def("commit", &OutputTransaction::Commit)
        .def("complete", &OutputTransaction::Complete)
        .def("rollback", &OutputTransaction::Rollback)
        .def("cleanup_external_run", &OutputTransaction::CleanupExternalRun)
        .def_property_readonly("active", &OutputTransaction::IsActive)
        .def("staged_outputs",
             [](const OutputTransaction &self)
             {
                 std::vector<std::pair<std::string, std::string>> result;
                 for (const auto &[final, staged] : self.StagedOutputs())
                     result.emplace_back(final.string(), staged.string());
                 return result;
             });
    py::class_<ExecutionContext>(m, "ExecutionContext")
        .def(py::init<>())
        .def("begin_run", &ExecutionContext::BeginRun)
        .def("complete_run", &ExecutionContext::CompleteRun)
        .def("rollback_run", &ExecutionContext::RollbackRun)
        .def("cleanup_external_run", &ExecutionContext::CleanupExternalRun)
        .def("set_cache_directory", [](ExecutionContext &self, const py::object &path)
             { self.SetCacheDirectory(PythonPathText(path)); })
        .def("set_output_directory", [](ExecutionContext &self, const py::object &path)
             { self.SetOutputDirectory(PythonPathText(path)); })
        .def("stage_output", [](ExecutionContext &self, const py::object &path)
             { return PythonPath(self.StageOutput(PythonPathText(path))); })
        .def("final_output", [](const ExecutionContext &self, const py::object &path)
             { return PythonPath(self.FinalOutput(PythonPathText(path))); })
        .def("snapshot_state",
             [](const ExecutionContext &self)
             {
                 py::dict result;
                 result["output_dir"] = self.OutputDirectory().string();
                 return result;
             })
        .def_property_readonly("cache_directory", [](const ExecutionContext &self) { return PythonPath(self.CacheDirectory()); })
        .def_property_readonly("output_directory", [](const ExecutionContext &self) { return PythonPath(self.OutputDirectory()); })
        .def_property_readonly("run_id", &ExecutionContext::RunId)
        .def_property_readonly("active", &ExecutionContext::IsActive)
        .def_property_readonly("cancellation", [](ExecutionContext &self) -> CancellationToken & { return self.Cancellation(); },
                               py::return_value_policy::reference_internal)
        .def_property_readonly("outputs", [](ExecutionContext &self) -> OutputTransaction & { return self.Outputs(); },
                               py::return_value_policy::reference_internal);
    py::class_<ModuleRunManifest>(m, "ModuleRunManifest")
        .def("to_json", &ModuleRunManifest::ToJSON, py::arg("indent") = 2)
        .def_property_readonly("manifest_path", [](const ModuleRunManifest &self) { return self.ManifestPath; });
    py::class_<ProvenanceRecorder>(m, "ProvenanceRecorder")
        .def_static("make_workflow_run_id", &ProvenanceRecorder::MakeWorkflowRunId)
        .def_static("begin_module_run", &ProvenanceRecorder::BeginModuleRun)
        .def_static("track_input",
                    [](const std::string &runId, const std::string &path) { ProvenanceRecorder::TrackInput(runId, path); })
        .def_static("set_cache_source", &ProvenanceRecorder::SetCacheSource)
        .def_static("set_plugin_origin",
                    [](const std::string &runId, const py::object &origin)
                    { ProvenanceRecorder::SetPluginOrigin(runId, PluginOriginFromPython(origin)); })
        .def_static("build_module_run",
                    [](const std::string &runId, const py::dict &metadataValue, const std::string &codeHash,
                       const std::string &snapshotHash, const py::object &parameters, const std::string &outputDirectory,
                       const std::string &cacheDirectory, const std::string &status, const std::string &phase,
                       const std::string &message, const std::vector<std::pair<std::string, std::string>> &stagedOutputs,
                       const std::string &manifestPath)
                    {
                        ModuleMetadata metadata;
                        metadata.Name = py::cast<std::string>(metadataValue["name"]);
                        metadata.Version = py::cast<std::string>(metadataValue["version"]);
                        metadata.Summary = py::cast<std::string>(metadataValue["summary"]);
                        metadata.Tags = py::cast<std::vector<std::string>>(metadataValue["tags"]);
                        RunResult result;
                        result.Status = StatusFromText(status);
                        result.Phase = PhaseFromText(phase);
                        result.Message = message;
                        const std::string parametersJson = py::module_::import("json").attr("dumps")(parameters).cast<std::string>();
                        std::vector<std::pair<std::filesystem::path, std::filesystem::path>> paths;
                        for (const auto &[final, staged] : stagedOutputs) paths.emplace_back(final, staged);
                        return ProvenanceRecorder::BuildModuleRun(runId, metadata, codeHash, snapshotHash,
                                                                  parametersJson, outputDirectory, cacheDirectory,
                                                                  result, paths, manifestPath);
                    })
        .def_static("write_module_run",
                    [](const ModuleRunManifest &manifest, const std::string &path)
                    { ProvenanceRecorder::WriteModuleRun(manifest, path); })
        .def_static("write_workflow_run",
                    [](const std::string &runId, const std::string &startedAt, const std::string &finishedAt,
                       const std::string &language, bool failFast, bool succeeded, const py::list &nodeValues,
                       const py::list &linkValues, const std::vector<std::string> &moduleManifests,
                       const std::string &path)
                    {
                        WorkflowRunManifest workflow;
                        workflow.RunId = runId;
                        workflow.StartedAt = startedAt.empty() ? ProvenanceRecorder::NowUTC() : startedAt;
                        workflow.FinishedAt = finishedAt.empty() ? ProvenanceRecorder::NowUTC() : finishedAt;
                        workflow.Runtime = ProvenanceRecorder::Runtime(language);
                        workflow.FailFast = failFast;
                        workflow.Succeeded = succeeded;
                        workflow.ModuleManifestPaths = moduleManifests;
                        for (const auto &value : nodeValues)
                        {
                            const py::dict node = py::cast<py::dict>(value);
                            WorkflowNodeProvenance result;
                            result.Name = py::cast<std::string>(node["name"]);
                            result.Status = py::cast<std::string>(node["status"]);
                            result.Message = py::cast<std::string>(node["message"]);
                            result.Dependencies = py::cast<std::vector<std::string>>(node["dependencies"]);
                            if (!node["module_run_id"].is_none())
                                result.ModuleRunId = py::cast<std::string>(node["module_run_id"]);
                            if (!node["module_manifest"].is_none())
                                result.ModuleManifestPath = py::cast<std::string>(node["module_manifest"]);
                            workflow.Nodes.push_back(std::move(result));
                        }
                        for (const auto &value : linkValues)
                        {
                            const py::dict link = py::cast<py::dict>(value);
                            workflow.DataLinks.push_back({py::cast<std::string>(link["from"]),
                                                          py::cast<std::string>(link["to"]),
                                                          py::cast<std::string>(link["label"])});
                        }
                        return ProvenanceRecorder::WriteWorkflowRun(std::move(workflow), path);
                    },
                    py::arg("run_id"), py::arg("started_at"), py::arg("finished_at"), py::arg("language"),
                    py::arg("fail_fast"), py::arg("succeeded"), py::arg("nodes"), py::arg("data_links"),
                    py::arg("module_manifests"), py::arg("path") = "")
        .def_static("discard_module_run", &ProvenanceRecorder::DiscardModuleRun);
    py::class_<AMCM>(m, "AMCM")
        .def(py::init<PluginTrustPolicy, bool>(), py::arg("trust_policy") = PluginTrustPolicy::Verified,
             py::arg("discover_plugins") = true)
        .def("register_module",
             [](AMCM &self, const std::string &base)
             {
                 py::gil_scoped_release release;
                 return self.RegisterModule(base);
             })
        .def("register_module",
             [](AMCM &self, const std::string &base, const std::string &name)
             {
                 py::gil_scoped_release release;
                 return self.RegisterModule(base, name);
             })
        .def("register_module_handle",
             [](AMCM &self, std::shared_ptr<IAnalysisModule> module)
             {
                 py::gil_scoped_release release;
                 return self.RegisterModuleHandle(std::move(module));
             })
        .def("get_list_available_modules", &AMCM::ListAvailableModules)
        .def("get_list_available_module_metadata", &AMCM::ListAvailableModuleMetadata)
        .def("get_plugin_origin", &AMCM::GetPluginOrigin)
        .def("get_plugin_trust_policy", &AMCM::GetPluginTrustPolicy)
        .def("refresh_plugins",
             [](AMCM &self)
             {
                 py::gil_scoped_release release;
                 return self.RefreshPlugins();
             })
        .def("get_list_registered_modules", &AMCM::ListRegisteredModules)
        .def("get_status", &AMCM::GetStatus)
        .def("get_module", &AMCM::GetModule, py::return_value_policy::reference_internal)
        .def("get_all_progress",
             [](const AMCM &self)
             {
                 py::gil_scoped_release release;
                 return self.GetAllProgress();
             })
        .def("save_run_log",
             [](const AMCM &self)
             {
                 py::gil_scoped_release release;
                 self.SaveRunLog();
             })
        .def("save_provenance",
             [](const AMCM &self, const std::string &path, bool failFast)
             {
                 py::gil_scoped_release release;
                 return self.SaveProvenance(path, failFast);
             },
             py::arg("path") = "", py::arg("fail_fast") = true)
        .def("run_module",
             [](AMCM &self, const std::string &name)
             {
                 py::gil_scoped_release release;
                 return self.RunAModule(name);
             })
        .def("run_module",
             [](AMCM &self, std::shared_ptr<IAnalysisModule> module)
             {
                 py::gil_scoped_release release;
                 return self.RunAModule(std::move(module));
             })
        .def("run_module_isolated",
             [](AMCM &self, const std::string &name)
             {
                 py::gil_scoped_release release;
                 return self.RunAModuleIsolated(name);
             })
        .def("run_module_isolated",
             [](AMCM &self, std::shared_ptr<IAnalysisModule> module)
             {
                 py::gil_scoped_release release;
                 return self.RunAModuleIsolated(std::move(module));
             })
        .def("sequential_run",
             [](AMCM &self, bool failFast)
             {
                 py::gil_scoped_release release;
                 return self.SequentialRun(failFast);
             },
             py::arg("fail_fast") = true)
        .def("run_group",
             [](AMCM &self, const std::vector<std::string> &group, bool failFast)
             {
                 py::gil_scoped_release release;
                 return self.RunModules(group, failFast);
             },
             py::arg("group"), py::arg("fail_fast") = true)
        .def("run_group",
             [](AMCM &self, std::vector<std::shared_ptr<IAnalysisModule>> group, bool failFast)
             {
                 py::gil_scoped_release release;
                 return self.RunModules(std::move(group), failFast);
             },
             py::arg("group"), py::arg("fail_fast") = true)
        .def("get_dag", &AMCM::GetDAGManager, py::return_value_policy::reference_internal)
        .def("add_module_to_dag",
             [](AMCM &self, const std::string &name, const std::vector<std::string> &dependencies, bool isolated)
             {
                 py::gil_scoped_release release;
                 self.AddModuleToDAG(name, dependencies, isolated);
             },
             py::arg("name"), py::arg("dependencies") = std::vector<std::string>{}, py::arg("isolated") = false)
        .def("link_dag_module_parameter",
             [](AMCM &self, const std::string &fromNode, const std::string &fromKey, const std::string &toNode,
                const std::string &toKey)
             {
                 py::gil_scoped_release release;
                 self.LinkDAGModuleParameter(fromNode, fromKey, toNode, toKey);
             })
        .def("run_dag",
             [](AMCM &self, bool failFast)
             {
                 py::gil_scoped_release release;
                 return self.RunDAG(failFast);
             },
             py::arg("fail_fast") = true);
    py::enum_<logger::LogLevel>(m, "log_level")
        .value("DEBUG", logger::LogLevel::DEBUG)
        .value("INFO", logger::LogLevel::INFO)
        .value("WARN", logger::LogLevel::WARN)
        .value("ERROR", logger::LogLevel::ERROR)
        .value("NONE", logger::LogLevel::NONE);
    m.def("set_log_level", [](logger::LogLevel level) { logger::Logger::Get().SetLogLevel(level); });
    m.def("set_log_file", [](const std::string &path) { logger::Logger::Get().InitLogFile(path); });
    m.def("log", [](logger::LogLevel level, const std::string &mod, const std::string &msg) { logger::Logger::Get().Log(level, mod, msg); });
    m.def("get_version", []() { return std::string(CascadeVersionString()); });
    m.def("get_version_major", []() { return CascadeVersionMajor(); });
    m.def("get_version_minor", []() { return CascadeVersionMinor(); });
    m.def("get_version_patch", []() { return CascadeVersionPatch(); });
    m.def("get_abi_version", []() { return CASCADE_PLUGIN_ABI_VERSION; });
    m.def("get_abi_tag", []() { return std::string(CASCADE_ABI_TAG); });
    m.def("init_interrupt", &InterruptManager::Init);
    m.def("is_interrupted", &InterruptManager::IsInterrupted);
    auto moduleStatus = py::enum_<ModuleStatus>(m, "ModuleStatus");
    moduleStatus
        .value("Pending", ModuleStatus::Pending)
        .value("PENDING", ModuleStatus::Pending)
        .value("Initializing", ModuleStatus::Initializing)
        .value("INITIALIZING", ModuleStatus::Initializing)
        .value("Running", ModuleStatus::Running)
        .value("RUNNING", ModuleStatus::Running)
        .value("Finalizing", ModuleStatus::Finalizing)
        .value("FINALIZING", ModuleStatus::Finalizing)
        .value("Done", ModuleStatus::Done)
        .value("DONE", ModuleStatus::Done)
        .value("Skipped", ModuleStatus::Skipped)
        .value("SKIPPED", ModuleStatus::Skipped)
        .value("Interrupted", ModuleStatus::Interrupted)
        .value("INTERRUPTED", ModuleStatus::Interrupted)
        .value("Failed", ModuleStatus::Failed)
        .value("FAILED", ModuleStatus::Failed)
        .def_property_readonly("value", [](ModuleStatus status) { return std::string(ToString(status)); });
    auto modulePhase = py::enum_<ModulePhase>(m, "ModulePhase");
    modulePhase
        .value("None_", ModulePhase::None)
        .value("NONE", ModulePhase::None)
        .value("Init", ModulePhase::Init)
        .value("INIT", ModulePhase::Init)
        .value("Check", ModulePhase::Check)
        .value("CHECK", ModulePhase::Check)
        .value("Execute", ModulePhase::Execute)
        .value("EXECUTE", ModulePhase::Execute)
        .value("Finalize", ModulePhase::Finalize)
        .value("FINALIZE", ModulePhase::Finalize)
        .value("Commit", ModulePhase::Commit)
        .value("COMMIT", ModulePhase::Commit)
        .def_property_readonly("value", [](ModulePhase phase) { return std::string(ToString(phase)); });
    py::class_<RunResult>(m, "RunResult")
        .def(py::init([](ModuleStatus status, ModulePhase phase, std::string message, const py::object &exception)
             {
                 RunResult result{status, phase, std::move(message), nullptr};
                 if (!exception.is_none())
                     result.Exception = std::make_exception_ptr(std::runtime_error(py::str(exception).cast<std::string>()));
                 return result;
             }),
             py::arg("status") = ModuleStatus::Pending, py::arg("phase") = ModulePhase::None,
             py::arg("message") = "", py::arg("exception") = py::none())
        .def_readonly("status", &RunResult::Status)
        .def_readonly("phase", &RunResult::Phase)
        .def_readonly("message", &RunResult::Message)
        .def_readonly("cache_decision", &RunResult::CacheDecision)
        .def_readonly("cache_reason", &RunResult::CacheReason)
        .def_property_readonly("exception",
             [](const RunResult &result) -> py::object
             {
                 if (!result.Exception) return py::none();
                 return py::module_::import("builtins").attr("RuntimeError")(
                     result.Message.empty() ? "Module execution failed" : result.Message);
             })
        .def("succeeded", &RunResult::Succeeded)
        .def("failed", &RunResult::Failed)
        .def("is_terminal", &RunResult::IsTerminal)
        .def("allows_dependents", &RunResult::AllowsDependents)
        .def("has_exception", &RunResult::HasException);
    py::enum_<DAGNodeStatus>(m, "DAGNodeStatus")
        .value("Pending", DAGNodeStatus::Pending)
        .value("Running", DAGNodeStatus::Running)
        .value("Succeeded", DAGNodeStatus::Succeeded)
        .value("Failed", DAGNodeStatus::Failed)
        .value("Blocked", DAGNodeStatus::Blocked);
    py::enum_<DAGExecutionLane>(m, "DAGExecutionLane")
        .value("Serial", DAGExecutionLane::Serial)
        .value("Parallel", DAGExecutionLane::Parallel)
        .value("Root", DAGExecutionLane::Root)
        .value("Isolated", DAGExecutionLane::Isolated);
    py::class_<DAGNodeResult>(m, "DAGNodeResult")
        .def_readonly("name", &DAGNodeResult::Name)
        .def_readonly("status", &DAGNodeResult::Status)
        .def_readonly("message", &DAGNodeResult::Message)
        .def("succeeded", &DAGNodeResult::Succeeded)
        .def("failed", &DAGNodeResult::Failed)
        .def("blocked", &DAGNodeResult::Blocked)
        .def("is_terminal", &DAGNodeResult::IsTerminal);
    py::class_<DAGRunResult>(m, "DAGRunResult")
        .def_readonly("nodes", &DAGRunResult::Nodes)
        .def("succeeded", &DAGRunResult::Succeeded)
        .def("failed", &DAGRunResult::Failed);
    py::class_<DAGDataLinkInfo>(m, "DAGDataLinkInfo")
        .def_readonly("from_node", &DAGDataLinkInfo::FromNode)
        .def_readonly("to_node", &DAGDataLinkInfo::ToNode)
        .def_readonly("label", &DAGDataLinkInfo::Label);
    py::class_<DAGManager>(m, "DAGManager")
        .def("add_node",
             [](DAGManager &dag, const std::string &name, const std::vector<std::string> &dependencies,
                std::function<void()> task, DAGExecutionLane lane)
             {
                 py::gil_scoped_release release;
                 dag.AddNode(name, dependencies, std::move(task), lane);
             },
             py::arg("name"), py::arg("dependencies"), py::arg("task"),
             py::arg("lane") = DAGExecutionLane::Serial)
        .def("add_data_link",
             [](DAGManager &dag, const std::string &fromNode, const std::string &toNode, const std::string &label,
                std::function<void()> transfer)
             {
                 py::gil_scoped_release release;
                 dag.AddDataLink(fromNode, toNode, label, std::move(transfer));
             })
        .def("validate",
             [](const DAGManager &dag)
             {
                 py::gil_scoped_release release;
                 dag.Validate();
             })
        .def("execute",
             [](DAGManager &dag, bool failFast)
             {
                 py::gil_scoped_release release;
                 return dag.Execute(failFast);
             },
             py::arg("fail_fast") = true)
        .def("reset",
             [](DAGManager &dag)
             {
                 py::gil_scoped_release release;
                 dag.Reset();
             })
        .def("reset_failed",
             [](DAGManager &dag)
             {
                 py::gil_scoped_release release;
                 dag.ResetFailed();
             })
        .def("dump_dot",
             [](const DAGManager &dag, const std::string &path)
             {
                 py::gil_scoped_release release;
                 dag.DumpDOT(path);
             })
        .def("get_node_names",
             [](const DAGManager &dag)
             {
                 py::gil_scoped_release release;
                 return dag.GetNodeNames();
             })
        .def("get_node_results",
             [](const DAGManager &dag)
             {
                 py::gil_scoped_release release;
                 return dag.GetNodeResults();
             })
        .def("get_dependencies", &DAGManager::GetDependencies)
        .def("get_data_links", &DAGManager::GetDataLinks)
        .def("is_executing", &DAGManager::IsExecuting);
    py::class_<IAnalysisModule, PythonAnalysisModule, std::shared_ptr<IAnalysisModule>>(m, "IAnalysisModule")
        .def(py::init<>())
        .def("run", [](IAnalysisModule &module) { py::gil_scoped_release release; return module.Run(); })
        .def("prepare_external_run",
             [](IAnalysisModule &module) { py::gil_scoped_release release; module.PrepareExternalRun(); })
        .def("prepare_external_run_with_id",
             [](IAnalysisModule &module, const std::string &runId)
             { py::gil_scoped_release release; module.PrepareExternalRunWithId(runId); })
        .def("run_prepared_external",
             [](IAnalysisModule &module) { py::gil_scoped_release release; return module.RunPreparedExternal(); })
        .def("adopt_external_run_result",
             [](IAnalysisModule &module, RunResult result)
             { py::gil_scoped_release release; return module.AdoptExternalRunResult(std::move(result)); })
        .def("register_param",
             [](IAnalysisModule &module, const std::string &key, const py::object &value,
                const std::string &description)
             {
                 ParamManager &parameters = module.GetParamManager();
                 if (parameters.Has(key)) throw py::key_error("Parameter already registered: " + key);
                 parameters.Register<ParamValue>(key, RegisteredParamValueFromPython(value), description);
             },
             py::arg("name"), py::arg("default"), py::arg("description") = "")
        .def("set_param",
             [](IAnalysisModule &module, const std::string &key, const py::object &value)
             {
                 if (!module.HasParam(key)) throw py::key_error("Parameter is not registered: " + key);
                 ParamValue converted = ParamValueFromPython(value);
                 try
                 {
                     py::gil_scoped_release release;
                     module.SetParamValue(key, converted);
                 }
                 catch (const std::exception &error)
                 {
                     throw py::type_error(error.what());
                 }
             })
        .def("set_param_from_dict",
             [](IAnalysisModule &module, const py::dict &values)
             {
                 std::vector<std::pair<std::string, ParamValue>> converted;
                 converted.reserve(values.size());
                 for (const auto &item : values)
                     converted.emplace_back(py::cast<std::string>(item.first), ParamValueFromPython(item.second));
                 py::gil_scoped_release release;
                 for (const auto &[key, value] : converted)
                     module.SetParamValue(key, value);
             })
        .def("get_param",
             [](const IAnalysisModule &module, const std::string &key, const py::object &fallback)
             { return module.HasParam(key) ? ParamValueToPython(module.GetParamValue(key)) : fallback; },
             py::arg("name"), py::arg("default") = py::none())
        .def("get_parameters", [](const IAnalysisModule &module) { return ParamManagerToPython(module.GetParamManager()); })
        .def("load_param_from_yaml",
             [](IAnalysisModule &module, const std::string &path)
             {
                 py::gil_scoped_release release;
                 module.LoadParamsFromYAML(path);
             })
        .def("load_params_from_json",
             [](IAnalysisModule &module, const std::string &path)
             {
                 py::gil_scoped_release release;
                 module.LoadParamsFromJSON(path);
             })
        .def("set_params_from_json",
             [](IAnalysisModule &module, const std::string &document)
             {
                 py::gil_scoped_release release;
                 module.SetParamsFromJSON(document);
             })
        .def("save_params_to_yaml",
             [](IAnalysisModule &module, const std::string &path)
             {
                 py::gil_scoped_release release;
                 module.SaveParamsToYAML(path);
             })
        .def("save_params_to_json",
             [](IAnalysisModule &module, const std::string &path)
             {
                 py::gil_scoped_release release;
                 module.SaveParamsToJSON(path);
             })
        .def("dump_params_to_yaml",
             [](IAnalysisModule &module, int indent)
             {
                 py::gil_scoped_release release;
                 return module.DumpParamsToYAML(indent);
             },
             py::arg("indent") = 2)
        .def("dump_params_to_json",
             [](IAnalysisModule &module, int indent)
             {
                 py::gil_scoped_release release;
                 return module.DumpParamsToJSON(indent);
             },
             py::arg("indent") = 4)
        .def("print_description", &IAnalysisModule::Description)
        .def("set_name", &IAnalysisModule::SetName)
        .def("name", &IAnalysisModule::Name)
        .def("get_basename", &IAnalysisModule::BaseName)
        .def("get_status", &IAnalysisModule::GetStatus)
        .def("get_status_enum", &IAnalysisModule::GetStatusEnum)
        .def("get_last_run_result", &IAnalysisModule::GetLastRunResult)
        .def("get_code_hash", &IAnalysisModule::GetCodeHash)
        .def("get_runtime_language", &IAnalysisModule::GetRuntimeLanguage)
        .def("requires_root_serialization", &IAnalysisModule::RequiresRootSerialization)
        .def("set_cache_directory", [](IAnalysisModule &module, const py::object &path)
             { module.SetCacheDirectory(PythonPathText(path)); })
        .def("set_output_directory", [](IAnalysisModule &module, const py::object &path)
             { module.SetOutputDirectory(PythonPathText(path)); })
        .def("get_cache_directory", &IAnalysisModule::GetCacheDirectory)
        .def("get_output_directory", &IAnalysisModule::GetOutputDirectory)
        .def("get_run_id", &IAnalysisModule::GetRunId)
        .def("get_last_provenance_path", &IAnalysisModule::GetLastProvenancePath)
        .def("get_last_provenance_json", &IAnalysisModule::GetLastProvenanceJSON, py::arg("indent") = 2)
        .def("request_cancellation", &IAnalysisModule::RequestCancellation)
        .def("is_cancellation_requested", &IAnalysisModule::IsCancellationRequested)
        .def("stage_output", [](IAnalysisModule &module, const py::object &path)
             { return PythonPath(module.GetExecutionContext().StageOutput(PythonPathText(path))); })
        .def("final_output", [](const IAnalysisModule &module, const py::object &path)
             { return PythonPath(module.GetExecutionContext().FinalOutput(PythonPathText(path))); })
        .def("track_input", [](IAnalysisModule &module, const py::object &path)
             {
                 if (!module.GetExecutionContext().IsActive())
                     throw std::runtime_error("Cannot track an input outside an active module run");
                 ProvenanceRecorder::TrackInput(module.GetRunId(), PythonPathText(path));
             })
        .def("set_plugin_origin", [](IAnalysisModule &module, const py::object &origin)
             { module.SetPluginOrigin(PluginOriginFromPython(origin)); })
        .def("set_status", [](IAnalysisModule &module, ModuleStatus status) { module.SetStatus(status); })
        .def("get_progress", &IAnalysisModule::GetProgressSnapshot)
        .def_property_readonly("params", [](IAnalysisModule &module) -> ParamManager & { return module.GetParamManager(); },
                               py::return_value_policy::reference_internal)
        .def_property_readonly("context", [](IAnalysisModule &module) -> ExecutionContext & { return module.GetExecutionContext(); },
                               py::return_value_policy::reference_internal)
        .def_property("basename", &IAnalysisModule::BaseName, &IAnalysisModule::SetBaseName)
        .def_property("code_version_hash", &IAnalysisModule::GetCodeHash, &IAnalysisModule::SetCodeHash)
        .def_property_readonly("status", &IAnalysisModule::GetStatusEnum)
        .def_property_readonly("last_run_result", &IAnalysisModule::GetLastRunResult)
        .def("get_metadata", &IAnalysisModule::GetMetadata);

    py::class_<ModuleMetadata>(m, "ModuleMetadata")
        .def_readonly("name", &ModuleMetadata::Name)
        .def_readonly("version", &ModuleMetadata::Version)
        .def_readonly("summary", &ModuleMetadata::Summary)
        .def_readonly("tags", &ModuleMetadata::Tags);
}
