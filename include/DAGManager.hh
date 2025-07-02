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
        std::string name;
        std::vector<std::string> dependencies;
        std::function<void()> task;
        bool executed = false;
    };

    void AddNode(const std::string &name, const std::vector<std::string> &deps,
                 std::function<void()> task);
    void LinkOutputToParam(const std::string &fromNode,
                           const std::string &outKey, const std::string &toNode,
                           const std::string &paramKey);
    void SetParamManagerMap(
        const std::unordered_map<std::string, ParamManager *> &map);
    void Execute();
    void DumpDOT(const std::string &filename) const;
    std::vector<std::string> GetNodeNames() const;

  private:
    std::unordered_map<std::string, Node> nodes;
    std::unordered_set<std::string> visited;
    std::unordered_set<std::string> recursionStack;

    struct ParamLink
    {
        std::string fromNode, fromKey;
        std::string toNode, toKey;
    };
    std::vector<ParamLink> paramLinks;
    std::unordered_map<std::string, ParamManager *> paramMap;

    void ExecuteNode(const std::string &name);
    void CheckForCycle(const std::string &name);
};