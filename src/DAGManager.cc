#include "DAGManager.hh"
#include <fstream>

void DAGManager::AddNode(const std::string &name, const std::vector<std::string> &deps, std::function<void()> task)
{
    if (nodes.count(name)) throw std::runtime_error("Node " + name + " already exists.");
    nodes[name] = Node{name, deps, task};
}

void DAGManager::LinkOutputToParam(const std::string &fromNode, const std::string &fromKey, const std::string &toNode, const std::string &toKey)
{
    paramLinks.push_back({fromNode, fromKey, toNode, toKey});
}

void DAGManager::SetParamManagerMap(const std::unordered_map<std::string, ParamManager *> &map)
{
    for (const auto &[name, ptr] : map)
        paramMap[name] = ptr;
}

void DAGManager::Execute()
{
    visited.clear();
    recursionStack.clear();

    for (auto &[name, node] : nodes)
    {
        if (!node.executed)
        {
            CheckForCycle(name);
            ExecuteNode(name);
        }
    }
}

void DAGManager::ExecuteNode(const std::string &name)
{
    auto &node = nodes[name];
    if (node.executed) return;
    for (const auto &dep : node.dependencies)
        ExecuteNode(dep);

    for (auto &link : paramLinks)
    {
        if (link.toNode == name)
        {
            if (!paramMap.count(link.fromNode) || !paramMap.count(link.toNode))
                throw std::runtime_error("ParamManager not found for node in link: " + link.fromNode + " or " + link.toNode);
            const auto &val = paramMap.at(link.fromNode)->GetRawAny(link.fromKey);
            paramMap[link.toNode]->SetParamFromAny(link.toKey, val);
        }
    }
    node.task();
    node.executed = true;
}

void DAGManager::CheckForCycle(const std::string &name)
{
    if (recursionStack.count(name)) throw std::runtime_error("Cycle detected at node: " + name);
    if (visited.count(name)) return;

    visited.insert(name);
    recursionStack.insert(name);

    for (const auto &dep : nodes.at(name).dependencies)
        CheckForCycle(dep);

    recursionStack.erase(name);
}

void DAGManager::DumpDOT(const std::string &filename) const
{
    std::ofstream fout(filename);
    if (!fout.is_open()) throw std::runtime_error("Failed to open file: " + filename);

    fout << "digraph DAG {\n";

    for (const auto &[name, node] : nodes)
    {
        fout << "    \"" << name << "\";\n";
        for (const auto &dep : node.dependencies)
        {
            fout << "    \"" << dep << "\" -> \"" << name << "\";\n";
        }
    }

    for (const auto &link : paramLinks)
    {
        fout << "    \"" << link.fromNode << "\" -> \"" << link.toNode << "\" [style=dotted, label=\"" << link.fromKey << "→" << link.toKey << "\"];\n";
    }

    fout << "}\n";
    fout.close();
}

std::vector<std::string> DAGManager::GetNodeNames() const
{
    std::vector<std::string> names;
    for (const auto &[name, _] : nodes)
    {
        names.push_back(name);
    }
    return names;
}