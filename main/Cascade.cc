#include "AMCM.hh"
#include "AnalysisManager.hh"
#include "AnalysisModuleRegistry.hh"
#include "DAGManager.hh"
#include "IAnalysisModule.hh"
#include "Logger.hh"
#include "ParamManager.hh"
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
        .def("get_list_registered_modules", &AMCM::ListRegisteredModules)
        .def("get_status", &AMCM::GetStatus)
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
    py::class_<DAGManager>(m, "DAGManager")
        .def("add_node", &DAGManager::AddNode)
        .def("link_output_to_param", &DAGManager::LinkOutputToParam)
        .def("dump_dot", &DAGManager::DumpDOT)
        .def("get_node_names", &DAGManager::GetNodeNames);
    py::class_<IAnalysisModule, std::shared_ptr<IAnalysisModule>>(m, "IAnalysisModule")
        .def("set_param", &IAnalysisModule::SetParamFromPy)
        .def("set_param_from_dict", &IAnalysisModule::SetParamsFromDict)
        .def("set_param_from_yaml", &IAnalysisModule::LoadParamsFromYAML)
        .def("get_parameters", &IAnalysisModule::GetParametersAsJSON)
        .def("print_description", &IAnalysisModule::Description)
        .def("name", &IAnalysisModule::Name);
}