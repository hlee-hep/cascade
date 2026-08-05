#include "AMCM.hh"
#include "BindingConversions.hh"
#include "Bindings.hh"
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
using namespace cascade::python_binding;

namespace
{
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

} // namespace

PYBIND11_MODULE(_cascade, m)
{
    BindPlugins(m);
    BindState(m);
    BindWorkflow(m);

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
