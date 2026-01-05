#include "ExampleModule.hh"
#include "AnalysisModuleMacros.hh"
#include "Logger.hh"

ExampleModule::ExampleModule()
{
    m_Basename = "ExampleModule";
    m_CodeVersionHash = "@VERSION_HASH@";
}

void ExampleModule::Description() const { LOG_INFO(m_Name, "ExampleModule: minimal C++ module for docs/examples"); }

void ExampleModule::Init() {}

void ExampleModule::Execute() {}

void ExampleModule::Finalize() {}

REGISTER_MODULE(ExampleModule);
