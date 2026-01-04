#pragma once
#include "ParamManager.hh"
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class DAGManager
{
  public:
    struct Node
    {
        std::string Name;
        std::vector<std::string> Dependencies;
        std::function<void()> Task;
        bool Executed = false;
    };

    void AddNode(const std::string &name, const std::vector<std::string> &deps, std::function<void()> task);
    void LinkOutputToParam(const std::string &fromNode, const std::string &outKey, const std::string &toNode, const std::string &paramKey);
    void SetParamManagerMap(const std::unordered_map<std::string, ParamManager *> &map);
    void Execute();
    void DumpDOT(const std::string &filename) const;
    std::vector<std::string> GetNodeNames() const;

  private:
    std::unordered_map<std::string, Node> m_Nodes;
    std::unordered_set<std::string> m_Visited;
    std::unordered_set<std::string> m_RecursionStack;

    struct ParamLink
    {
        std::string FromNode;
        std::string FromKey;
        std::string ToNode;
        std::string ToKey;
    };
    std::vector<ParamLink> m_ParamLinks;
    std::unordered_map<std::string, ParamManager *> m_ParamMap;

    void ExecuteNode_(const std::string &name);
    void CheckForCycle_(const std::string &name);
};
