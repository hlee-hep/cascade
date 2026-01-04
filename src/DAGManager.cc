#include "DAGManager.hh"
#include <fstream>

void DAGManager::AddNode(const std::string &name, const std::vector<std::string> &deps, std::function<void()> task)
{
    if (m_Nodes.count(name)) throw std::runtime_error("Node " + name + " already exists.");
    m_Nodes[name] = Node{name, deps, task};
}

void DAGManager::LinkOutputToParam(const std::string &fromNode, const std::string &fromKey, const std::string &toNode, const std::string &toKey)
{
    m_ParamLinks.push_back({fromNode, fromKey, toNode, toKey});
}

void DAGManager::SetParamManagerMap(const std::unordered_map<std::string, ParamManager *> &map)
{
    for (const auto &[name, ptr] : map)
        m_ParamMap[name] = ptr;
}

void DAGManager::Execute()
{
    m_Visited.clear();
    m_RecursionStack.clear();

    for (auto &[name, node] : m_Nodes)
    {
        if (!node.Executed)
        {
            CheckForCycle_(name);
            ExecuteNode_(name);
        }
    }
}

void DAGManager::ExecuteNode_(const std::string &name)
{
    auto &node = m_Nodes[name];
    if (node.Executed) return;
    for (const auto &dep : node.Dependencies)
        ExecuteNode_(dep);

    for (auto &link : m_ParamLinks)
    {
        if (link.ToNode == name)
        {
            if (!m_ParamMap.count(link.FromNode) || !m_ParamMap.count(link.ToNode))
                throw std::runtime_error("ParamManager not found for node in link: " + link.FromNode + " or " + link.ToNode);
            const auto val = m_ParamMap.at(link.FromNode)->Get<ParamValue>(link.FromKey);
            m_ParamMap[link.ToNode]->SetParamVariant(link.ToKey, val);
        }
    }
    node.Task();
    node.Executed = true;
}

void DAGManager::CheckForCycle_(const std::string &name)
{
    if (m_RecursionStack.count(name)) throw std::runtime_error("Cycle detected at node: " + name);
    if (m_Visited.count(name)) return;

    m_Visited.insert(name);
    m_RecursionStack.insert(name);

    for (const auto &dep : m_Nodes.at(name).Dependencies)
        CheckForCycle_(dep);

    m_RecursionStack.erase(name);
}

void DAGManager::DumpDOT(const std::string &filename) const
{
    std::ofstream fout(filename);
    if (!fout.is_open()) throw std::runtime_error("Failed to open file: " + filename);

    fout << "digraph DAG {\n";

    for (const auto &[name, node] : m_Nodes)
    {
        fout << "    \"" << name << "\";\n";
        for (const auto &dep : node.Dependencies)
        {
            fout << "    \"" << dep << "\" -> \"" << name << "\";\n";
        }
    }

    for (const auto &link : m_ParamLinks)
    {
        fout << "    \"" << link.FromNode << "\" -> \"" << link.ToNode << "\" [style=dotted, label=\"" << link.FromKey << "→" << link.ToKey << "\"];\n";
    }

    fout << "}\n";
    fout.close();
}

std::vector<std::string> DAGManager::GetNodeNames() const
{
    std::vector<std::string> names;
    for (const auto &[name, _] : m_Nodes)
    {
        names.push_back(name);
    }
    return names;
}
