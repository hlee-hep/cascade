#include "BindingConversions.hh"
#include "Bindings.hh"
#include "CacheManager.hh"
#include "ExecutionContext.hh"
#include "Provenance.hh"
#include "SnapshotHasher.hh"

namespace py = pybind11;

namespace cascade::python_binding
{
void BindState(py::module_ &m)
{
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
}
} // namespace cascade::python_binding
