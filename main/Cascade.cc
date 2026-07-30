#include "AMCM.hh"
#include "AnalysisManager.hh"
#include "AnalysisModuleRegistry.hh"
#include "DAGManager.hh"
#include "IAnalysisModule.hh"
#include "InterruptManager.hh"
#include "Logger.hh"
#include "ParamManager.hh"
#include "PluginABI.hh"
#include "Version.hh"
#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

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
} // namespace

PYBIND11_MODULE(_cascade, m)
{
    py::class_<AMCM>(m, "AMCM")
        .def(py::init<>())
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
        .def("get_list_available_modules", &AMCM::ListAvailableModules)
        .def("get_list_available_module_metadata", &AMCM::ListAvailableModuleMetadata)
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
    py::enum_<ModuleStatus>(m, "ModuleStatus")
        .value("Pending", ModuleStatus::Pending)
        .value("Initializing", ModuleStatus::Initializing)
        .value("Running", ModuleStatus::Running)
        .value("Finalizing", ModuleStatus::Finalizing)
        .value("Done", ModuleStatus::Done)
        .value("Skipped", ModuleStatus::Skipped)
        .value("Interrupted", ModuleStatus::Interrupted)
        .value("Failed", ModuleStatus::Failed);
    py::enum_<ModulePhase>(m, "ModulePhase")
        .value("None_", ModulePhase::None)
        .value("Init", ModulePhase::Init)
        .value("Check", ModulePhase::Check)
        .value("Execute", ModulePhase::Execute)
        .value("Finalize", ModulePhase::Finalize)
        .value("Commit", ModulePhase::Commit);
    py::class_<RunResult>(m, "RunResult")
        .def_readonly("status", &RunResult::Status)
        .def_readonly("phase", &RunResult::Phase)
        .def_readonly("message", &RunResult::Message)
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
    py::class_<DAGManager>(m, "DAGManager")
        .def("add_node",
             [](DAGManager &dag, const std::string &name, const std::vector<std::string> &dependencies, std::function<void()> task)
             {
                 py::gil_scoped_release release;
                 dag.AddNode(name, dependencies, std::move(task));
             })
        .def("add_data_link",
             [](DAGManager &dag, const std::string &fromNode, const std::string &toNode, const std::string &label,
                std::function<void()> transfer)
             {
                 py::gil_scoped_release release;
                 dag.AddDataLink(fromNode, toNode, label, std::move(transfer));
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
        .def("is_executing", &DAGManager::IsExecuting);
    py::class_<IAnalysisModule, std::shared_ptr<IAnalysisModule>>(m, "IAnalysisModule")
        .def("set_param",
             [](IAnalysisModule &module, const std::string &key, const py::object &value)
             {
                 ParamValue converted = ParamValueFromPython(value);
                 py::gil_scoped_release release;
                 module.SetParamValue(key, converted);
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
        .def("name", &IAnalysisModule::Name)
        .def("get_basename", &IAnalysisModule::BaseName)
        .def("get_status", &IAnalysisModule::GetStatus)
        .def("get_status_enum", &IAnalysisModule::GetStatusEnum)
        .def("get_last_run_result", &IAnalysisModule::GetLastRunResult)
        .def("get_code_hash", &IAnalysisModule::GetCodeHash)
        .def("set_cache_directory", &IAnalysisModule::SetCacheDirectory)
        .def("set_output_directory", &IAnalysisModule::SetOutputDirectory)
        .def("get_cache_directory", &IAnalysisModule::GetCacheDirectory)
        .def("get_output_directory", &IAnalysisModule::GetOutputDirectory)
        .def("get_run_id", &IAnalysisModule::GetRunId)
        .def("request_cancellation", &IAnalysisModule::RequestCancellation)
        .def("is_cancellation_requested", &IAnalysisModule::IsCancellationRequested)
        .def("get_metadata", &IAnalysisModule::GetMetadata);

    py::class_<ModuleMetadata>(m, "ModuleMetadata")
        .def_readonly("name", &ModuleMetadata::Name)
        .def_readonly("version", &ModuleMetadata::Version)
        .def_readonly("summary", &ModuleMetadata::Summary)
        .def_readonly("tags", &ModuleMetadata::Tags);
}
