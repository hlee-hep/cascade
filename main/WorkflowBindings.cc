#include "AMCM.hh"
#include "Bindings.hh"
#include "DAGManager.hh"
#include "IAnalysisModule.hh"
#include "InterruptManager.hh"
#include "Logger.hh"
#include "PluginABI.hh"
#include "Version.hh"
#include <pybind11/functional.h>
#include <pybind11/stl.h>

namespace py = pybind11;

namespace cascade::python_binding
{
void BindWorkflow(py::module_ &m)
{
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
             },
             py::keep_alive<1, 2>())
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
        .value("WARNING", logger::LogLevel::WARN)
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
}
} // namespace cascade::python_binding
