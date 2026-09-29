#include "AMCM.hh"

#include "Logger.hh"
#include "Provenance.hh"

#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>
#include <set>

std::string AMCM::SaveProvenance(const std::string &path, bool failFast) const
{
    std::lock_guard<std::mutex> lock(m_ControlMutex);
    WorkflowRunManifest workflow;
    workflow.RunId = ProvenanceRecorder::MakeWorkflowRunId();
    workflow.FailFast = failFast;
    workflow.Succeeded = true;
    std::set<std::string> languages;

    std::map<std::string, ModuleRunManifest> manifestsByInstance;
    for (const auto &entry : m_ExecutedModules)
    {
        std::optional<ModuleRunManifest> manifest = ProvenanceRecorder::FindModuleRun(entry.RunId);
        if (!manifest && !entry.ManifestPath.empty() && std::filesystem::is_regular_file(entry.ManifestPath))
            manifest = ProvenanceRecorder::LoadModuleRun(entry.ManifestPath);
        if (manifest)
        {
            if (!manifest->Runtime.Language.empty()) languages.insert(manifest->Runtime.Language);
            manifestsByInstance[entry.InstanceName] = *manifest;
            workflow.ModuleManifestPaths.push_back(manifest->ManifestPath);
            if (workflow.StartedAt.empty() || manifest->StartedAt < workflow.StartedAt) workflow.StartedAt = manifest->StartedAt;
            if (workflow.FinishedAt.empty() || manifest->FinishedAt > workflow.FinishedAt) workflow.FinishedAt = manifest->FinishedAt;
        }
        workflow.Succeeded = workflow.Succeeded && entry.Result.AllowsDependents();
    }

    const auto dependencies = m_Dag->GetDependencies();
    const auto dagResults = m_Dag->GetNodeResults();
    if (!dagResults.empty())
    {
        for (const auto &result : dagResults)
        {
            WorkflowNodeProvenance node;
            node.Name = result.Name;
            node.Status = ToString(result.Status);
            node.Message = result.Message;
            const auto dependency = dependencies.find(result.Name);
            if (dependency != dependencies.end()) node.Dependencies = dependency->second;
            const auto module = manifestsByInstance.find(result.Name);
            if (module != manifestsByInstance.end())
            {
                node.ModuleRunId = module->second.RunId;
                node.ModuleManifestPath = module->second.ManifestPath;
            }
            workflow.Nodes.push_back(std::move(node));
        }
        for (const auto &link : m_Dag->GetDataLinks())
            workflow.DataLinks.push_back({link.FromNode, link.ToNode, link.Label});
        workflow.Succeeded = workflow.Succeeded &&
                             std::all_of(dagResults.begin(), dagResults.end(),
                                         [](const DAGNodeResult &node) { return node.Status == DAGNodeStatus::Succeeded; });
    }
    else
    {
        for (const auto &entry : m_ExecutedModules)
        {
            WorkflowNodeProvenance node;
            node.Name = entry.InstanceName;
            node.Status = ToString(entry.Result.Status);
            node.Message = entry.Result.Message;
            node.ModuleRunId = entry.RunId;
            node.ModuleManifestPath = entry.ManifestPath;
            workflow.Nodes.push_back(std::move(node));
        }
    }

    if (workflow.StartedAt.empty()) workflow.StartedAt = ProvenanceRecorder::NowUTC();
    if (workflow.FinishedAt.empty()) workflow.FinishedAt = ProvenanceRecorder::NowUTC();
    workflow.Runtime = ProvenanceRecorder::Runtime(
        languages.size() > 1 ? "mixed" : (languages.empty() ? "cpp" : *languages.begin()));
    const std::string saved = ProvenanceRecorder::WriteWorkflowRun(workflow, path);
    LOG_INFO("CONTROL", "Workflow provenance '" << saved << "' is saved.");
    return saved;
}
