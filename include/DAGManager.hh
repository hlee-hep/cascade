#pragma once
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

enum class DAGNodeStatus
{
    Pending,
    Running,
    Succeeded,
    Failed,
    Blocked
};

inline const char *ToString(DAGNodeStatus status)
{
    switch (status)
    {
    case DAGNodeStatus::Pending:
        return "Pending";
    case DAGNodeStatus::Running:
        return "Running";
    case DAGNodeStatus::Succeeded:
        return "Succeeded";
    case DAGNodeStatus::Failed:
        return "Failed";
    case DAGNodeStatus::Blocked:
        return "Blocked";
    }
    return "Unknown";
}

struct DAGNodeResult
{
    std::string Name;
    DAGNodeStatus Status = DAGNodeStatus::Pending;
    std::string Message;

    bool Succeeded() const { return Status == DAGNodeStatus::Succeeded; }
    bool Failed() const { return Status == DAGNodeStatus::Failed; }
    bool Blocked() const { return Status == DAGNodeStatus::Blocked; }
    bool IsTerminal() const
    {
        return Status == DAGNodeStatus::Succeeded || Status == DAGNodeStatus::Failed || Status == DAGNodeStatus::Blocked;
    }
};

struct DAGRunResult
{
    std::vector<DAGNodeResult> Nodes;

    bool Succeeded() const;
    bool Failed() const;
};

struct DAGDataLinkInfo
{
    std::string FromNode;
    std::string ToNode;
    std::string Label;
};

class DAGManager
{
  public:
    using Task = std::function<void()>;
    using DataTransfer = std::function<void()>;

    struct Node
    {
        std::string Name;
        std::vector<std::string> Dependencies;
        Task Action;
        DAGNodeStatus Status = DAGNodeStatus::Pending;
        std::string Message;
    };

    void AddNode(const std::string &name, const std::vector<std::string> &dependencies, Task task);
    void AddDataLink(const std::string &fromNode, const std::string &toNode, const std::string &label, DataTransfer transfer);
    void Validate() const;
    DAGRunResult Execute(bool failFast = true);
    void Reset();
    void ResetFailed();
    void DumpDOT(const std::string &filename) const;
    std::vector<std::string> GetNodeNames() const;
    std::vector<DAGNodeResult> GetNodeResults() const;
    std::map<std::string, std::vector<std::string>> GetDependencies() const;
    std::vector<DAGDataLinkInfo> GetDataLinks() const;
    bool IsExecuting() const;

  private:
    mutable std::recursive_mutex m_Mutex;
    bool m_Executing = false;
    std::map<std::string, Node> m_Nodes;

    struct DataLink
    {
        std::string FromNode;
        std::string ToNode;
        std::string Label;
        DataTransfer Transfer;
    };
    std::vector<DataLink> m_DataLinks;

    void Validate_() const;
    std::vector<std::string> TopologicalOrder_() const;
    bool DependsOn_(const std::string &node, const std::string &dependency) const;
    void MarkBlockedDescendants_(const std::string &failedNode);
};
