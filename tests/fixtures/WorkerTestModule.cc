#include "IAnalysisModule.hh"
#include "PluginABI.hh"

#include <fstream>

class WorkerTestModule final : public IAnalysisModule
{
  public:
    WorkerTestModule()
    {
        m_Basename = "WorkerTestModule";
        m_CodeVersionHash = "worker-test-v1";
        m_Param.Set("force_run", true);
    }

    void Description() const override {}

  protected:
    bool UsesAnalysisManagers() const override { return false; }
    void Init() override {}
    void Execute() override
    {
        std::ofstream output(StageOutput("worker-result.txt"));
        if (!output) throw std::runtime_error("Cannot create worker test output");
        output << "exec-worker";
    }
    void Finalize() override {}
};

CASCADE_PLUGIN_EXPORT_ABI
CASCADE_PLUGIN_EXPORT void CascadeRegisterPlugin() { CASCADE_REGISTER_MODULE(WorkerTestModule); }
