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

PYBIND11_MODULE(_cascade, m)
{
    py::class_<AMCM>(m, "AMCM")
        .def(py::init<>())
        .def("register_module", py::overload_cast<const std::string &>(&AMCM::RegisterModule), py::return_value_policy::reference_internal)
        .def("register_module", py::overload_cast<const std::string &, const std::string &>(&AMCM::RegisterModule), py::return_value_policy::reference_internal)
        .def("get_list_available_modules", &AMCM::ListAvailableModules)
        .def("get_list_available_module_metadata", &AMCM::ListAvailableModuleMetadata)
        .def("get_list_registered_modules", &AMCM::ListRegisteredModules)
        .def("get_status", &AMCM::GetStatus)
        .def("get_module", &AMCM::GetModule, py::return_value_policy::reference_internal)
        .def("get_all_progress", &AMCM::GetAllProgress)
        .def("save_run_log", &AMCM::SaveRunLog)
        .def("run_module", py::overload_cast<const std::string &>(&AMCM::RunAModule))
        .def("run_module", py::overload_cast<std::shared_ptr<IAnalysisModule>>(&AMCM::RunAModule))
        .def("sequential_run", &AMCM::SequentialRun)
        .def("run_group", py::overload_cast<const std::vector<std::string> &>(&AMCM::RunModules))
        .def("run_group", py::overload_cast<std::vector<std::shared_ptr<IAnalysisModule>>>(&AMCM::RunModules))
        .def("get_dag", &AMCM::GetDAGManager, py::return_value_policy::reference_internal)
        .def("run_dag", &AMCM::RunDAG);
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
    m.def("init_interrupt", &InterruptManager::Init);
    m.def("is_interrupted", &InterruptManager::IsInterrupted);
    py::class_<DAGManager>(m, "DAGManager")
        .def("add_node", &DAGManager::AddNode)
        .def("link_output_to_param", &DAGManager::LinkOutputToParam)
        .def("dump_dot", &DAGManager::DumpDOT)
        .def("get_node_names", &DAGManager::GetNodeNames);
    py::class_<IAnalysisModule, std::shared_ptr<IAnalysisModule>>(m, "IAnalysisModule")
        .def("set_param", &IAnalysisModule::SetParamFromPy)
        .def("set_param_from_dict", &IAnalysisModule::SetParamsFromDict)
        .def("load_param_from_yaml", &IAnalysisModule::LoadParamsFromYAML)
        .def("load_params_from_json", &IAnalysisModule::LoadParamsFromJSON)
        .def("save_params_to_yaml", &IAnalysisModule::SaveParamsToYAML)
        .def("save_params_to_json", &IAnalysisModule::DumpParamsToJSON)
        .def("dump_params_to_yaml", &IAnalysisModule::DumpParamsToYAML)
        .def("dump_params_to_json", &IAnalysisModule::DumpParamsToJSON)
        .def("print_description", &IAnalysisModule::Description)
        .def("name", &IAnalysisModule::Name)
        .def("get_basename", &IAnalysisModule::BaseName)
        .def("get_status", &IAnalysisModule::GetStatus)
        .def("get_code_hash", &IAnalysisModule::GetCodeHash)
        .def("get_metadata", &IAnalysisModule::GetMetadata);

    py::class_<ModuleMetadata>(m, "ModuleMetadata")
        .def_readonly("name", &ModuleMetadata::Name)
        .def_readonly("version", &ModuleMetadata::Version)
        .def_readonly("summary", &ModuleMetadata::Summary)
        .def_readonly("tags", &ModuleMetadata::Tags);
}
