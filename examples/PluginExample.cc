#include "PluginABI.hh"
#include "IAnalysisModule.hh"
#include "Logger.hh"

class ExamplePluginModule : public IAnalysisModule
{
  public:
    ExamplePluginModule()
    {
        m_Basename = "ExamplePluginModule";
        m_CodeVersionHash = "example";
    }

    void Description() const override
    {
        LOG_INFO(m_Name, "Example plugin module loaded.");
    }

    void Init() override {}
    void Execute() override {}
    void Finalize() override {}
};

CASCADE_PLUGIN_EXPORT_ABI
CASCADE_PLUGIN_EXPORT void CascadeRegisterPlugin() { CASCADE_REGISTER_MODULE(ExamplePluginModule); }
