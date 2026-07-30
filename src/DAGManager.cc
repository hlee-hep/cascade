#include "DAGManager.hh"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>
#include <unordered_map>

namespace
{
std::string EscapeDot(std::string value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (const char character : value)
    {
        if (character == '\\' || character == '"') escaped.push_back('\\');
        if (character == '\n')
            escaped += "\\n";
        else if (character != '\r')
            escaped.push_back(character);
    }
    return escaped;
}
} // namespace

bool DAGRunResult::Succeeded() const
{
    return std::all_of(Nodes.begin(), Nodes.end(), [](const DAGNodeResult &node) { return node.Status == DAGNodeStatus::Succeeded; });
}

bool DAGRunResult::Failed() const
{
    return std::any_of(Nodes.begin(), Nodes.end(),
                       [](const DAGNodeResult &node)
                       { return node.Status == DAGNodeStatus::Failed || node.Status == DAGNodeStatus::Blocked; });
}

void DAGManager::AddNode(const std::string &name, const std::vector<std::string> &dependencies, Task task)
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    if (m_Executing) throw std::runtime_error("Cannot add a DAG node while the DAG is executing.");
    if (name.empty()) throw std::invalid_argument("DAG node name cannot be empty.");
    if (!task) throw std::invalid_argument("DAG node has no task: " + name);
    if (m_Nodes.count(name)) throw std::runtime_error("DAG node already exists: " + name);

    std::set<std::string> uniqueDependencies;
    for (const auto &dependency : dependencies)
    {
        if (dependency.empty()) throw std::invalid_argument("DAG dependency name cannot be empty.");
        if (dependency == name) throw std::invalid_argument("DAG node cannot depend on itself: " + name);
        if (!uniqueDependencies.insert(dependency).second)
            throw std::invalid_argument("Duplicate DAG dependency: " + name + " -> " + dependency);
    }
    m_Nodes.emplace(name, Node{name, dependencies, std::move(task)});
}

void DAGManager::AddDataLink(const std::string &fromNode, const std::string &toNode, const std::string &label, DataTransfer transfer)
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    if (m_Executing) throw std::runtime_error("Cannot add a DAG data link while the DAG is executing.");
    if (fromNode.empty() || toNode.empty()) throw std::invalid_argument("DAG data-link node names cannot be empty.");
    if (fromNode == toNode) throw std::invalid_argument("DAG data link cannot target its source node: " + fromNode);
    if (label.empty()) throw std::invalid_argument("DAG data-link label cannot be empty.");
    if (!transfer) throw std::invalid_argument("DAG data link has no transfer callback: " + label);
    for (const auto &link : m_DataLinks)
        if (link.FromNode == fromNode && link.ToNode == toNode && link.Label == label)
            throw std::runtime_error("Duplicate DAG data link: " + fromNode + " -> " + toNode + " (" + label + ")");
    m_DataLinks.push_back({fromNode, toNode, label, std::move(transfer)});
}

DAGRunResult DAGManager::Execute(bool failFast)
{
    std::vector<std::string> order;
    {
        std::lock_guard<std::recursive_mutex> lock(m_Mutex);
        if (m_Executing) throw std::runtime_error("DAG execution is already in progress.");
        Validate_();
        order = TopologicalOrder_();
        m_Executing = true;
    }
    try
    {
        for (const auto &name : order)
        {
            Task action;
            std::vector<std::pair<std::string, DataTransfer>> transfers;
            {
                std::lock_guard<std::recursive_mutex> lock(m_Mutex);
                auto &node = m_Nodes.at(name);
                if (node.Status != DAGNodeStatus::Pending) continue;

                const auto failedDependency =
                    std::find_if(node.Dependencies.begin(), node.Dependencies.end(),
                                 [&](const std::string &dependency)
                                 {
                                     const auto status = m_Nodes.at(dependency).Status;
                                     return status == DAGNodeStatus::Failed || status == DAGNodeStatus::Blocked;
                                 });
                if (failedDependency != node.Dependencies.end())
                {
                    node.Status = DAGNodeStatus::Blocked;
                    node.Message = "Blocked by dependency: " + *failedDependency;
                    continue;
                }

                const auto incompleteDependency =
                    std::find_if(node.Dependencies.begin(), node.Dependencies.end(),
                                 [&](const std::string &dependency)
                                 { return m_Nodes.at(dependency).Status != DAGNodeStatus::Succeeded; });
                if (incompleteDependency != node.Dependencies.end())
                    throw std::logic_error("DAG topological execution reached an incomplete dependency: " + *incompleteDependency);

                node.Status = DAGNodeStatus::Running;
                node.Message.clear();
                action = node.Action;
                for (const auto &link : m_DataLinks)
                    if (link.ToNode == name) transfers.emplace_back(link.Label, link.Transfer);
            }

            try
            {
                for (const auto &[label, transfer] : transfers)
                {
                    try
                    {
                        transfer();
                    }
                    catch (const std::exception &error)
                    {
                        throw std::runtime_error("Data link '" + label + "' failed: " + error.what());
                    }
                    catch (...)
                    {
                        throw std::runtime_error("Data link '" + label + "' failed with an unknown exception");
                    }
                }
                action();
                std::lock_guard<std::recursive_mutex> lock(m_Mutex);
                m_Nodes.at(name).Status = DAGNodeStatus::Succeeded;
            }
            catch (const std::exception &error)
            {
                std::lock_guard<std::recursive_mutex> lock(m_Mutex);
                auto &node = m_Nodes.at(name);
                node.Status = DAGNodeStatus::Failed;
                node.Message = error.what();
                if (failFast)
                {
                    MarkBlockedDescendants_(name);
                    break;
                }
            }
            catch (...)
            {
                std::lock_guard<std::recursive_mutex> lock(m_Mutex);
                auto &node = m_Nodes.at(name);
                node.Status = DAGNodeStatus::Failed;
                node.Message = "Unknown task exception";
                if (failFast)
                {
                    MarkBlockedDescendants_(name);
                    break;
                }
            }
        }
        std::lock_guard<std::recursive_mutex> lock(m_Mutex);
        m_Executing = false;
    }
    catch (...)
    {
        std::lock_guard<std::recursive_mutex> lock(m_Mutex);
        for (auto &[_, node] : m_Nodes)
            if (node.Status == DAGNodeStatus::Running)
            {
                node.Status = DAGNodeStatus::Failed;
                node.Message = "DAG execution aborted";
            }
        m_Executing = false;
        throw;
    }
    return {GetNodeResults()};
}

void DAGManager::Reset()
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    if (m_Executing) throw std::runtime_error("Cannot reset the DAG while it is executing.");
    for (auto &[_, node] : m_Nodes)
    {
        node.Status = DAGNodeStatus::Pending;
        node.Message.clear();
    }
}

void DAGManager::ResetFailed()
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    if (m_Executing) throw std::runtime_error("Cannot reset the DAG while it is executing.");
    for (auto &[_, node] : m_Nodes)
        if (node.Status == DAGNodeStatus::Failed || node.Status == DAGNodeStatus::Blocked)
        {
            node.Status = DAGNodeStatus::Pending;
            node.Message.clear();
        }
}

void DAGManager::Validate_() const
{
    for (const auto &[name, node] : m_Nodes)
    {
        if (!node.Action) throw std::runtime_error("DAG node has no task: " + name);
        for (const auto &dependency : node.Dependencies)
            if (!m_Nodes.count(dependency)) throw std::runtime_error("DAG node '" + name + "' depends on missing node '" + dependency + "'.");
    }

    TopologicalOrder_();
    for (const auto &link : m_DataLinks)
    {
        if (!m_Nodes.count(link.FromNode)) throw std::runtime_error("DAG data-link source node is missing: " + link.FromNode);
        if (!m_Nodes.count(link.ToNode)) throw std::runtime_error("DAG data-link target node is missing: " + link.ToNode);
        if (!link.Transfer) throw std::runtime_error("DAG data link has no transfer callback: " + link.Label);
        if (!DependsOn_(link.ToNode, link.FromNode))
            throw std::runtime_error("DAG data-link source must be a dependency of its target: " + link.FromNode + " -> " + link.ToNode);
    }
}

std::vector<std::string> DAGManager::TopologicalOrder_() const
{
    enum class VisitState
    {
        Unvisited,
        Visiting,
        Visited
    };
    std::unordered_map<std::string, VisitState> states;
    std::vector<std::string> order;
    order.reserve(m_Nodes.size());

    std::function<void(const std::string &)> visit = [&](const std::string &name)
    {
        const auto state = states[name];
        if (state == VisitState::Visiting) throw std::runtime_error("Cycle detected at DAG node: " + name);
        if (state == VisitState::Visited) return;
        states[name] = VisitState::Visiting;
        for (const auto &dependency : m_Nodes.at(name).Dependencies)
        {
            if (!m_Nodes.count(dependency)) continue;
            visit(dependency);
        }
        states[name] = VisitState::Visited;
        order.push_back(name);
    };

    for (const auto &[name, _] : m_Nodes)
        visit(name);
    return order;
}

bool DAGManager::DependsOn_(const std::string &node, const std::string &dependency) const
{
    std::set<std::string> visited;
    std::function<bool(const std::string &)> search = [&](const std::string &current)
    {
        if (!visited.insert(current).second) return false;
        const auto iterator = m_Nodes.find(current);
        if (iterator == m_Nodes.end()) return false;
        for (const auto &candidate : iterator->second.Dependencies)
        {
            if (candidate == dependency || search(candidate)) return true;
        }
        return false;
    };
    return search(node);
}

void DAGManager::MarkBlockedDescendants_(const std::string &failedNode)
{
    for (auto &[name, node] : m_Nodes)
    {
        if (node.Status != DAGNodeStatus::Pending || !DependsOn_(name, failedNode)) continue;
        node.Status = DAGNodeStatus::Blocked;
        node.Message = "Blocked by dependency: " + failedNode;
    }
}

void DAGManager::DumpDOT(const std::string &filename) const
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    std::ofstream output(filename);
    if (!output) throw std::runtime_error("Failed to open DAG DOT file: " + filename);

    output << "digraph DAG {\n";
    for (const auto &[name, node] : m_Nodes)
    {
        const char *color = "gray";
        if (node.Status == DAGNodeStatus::Running)
            color = "gold";
        else if (node.Status == DAGNodeStatus::Succeeded)
            color = "forestgreen";
        else if (node.Status == DAGNodeStatus::Failed)
            color = "firebrick";
        else if (node.Status == DAGNodeStatus::Blocked)
            color = "darkorange";
        output << "    \"" << EscapeDot(name) << "\" [label=\"" << EscapeDot(name) << "\\n" << ToString(node.Status) << "\", color=\"" << color
               << "\"];\n";
        for (const auto &dependency : node.Dependencies)
            output << "    \"" << EscapeDot(dependency) << "\" -> \"" << EscapeDot(name) << "\";\n";
    }
    for (const auto &link : m_DataLinks)
        output << "    \"" << EscapeDot(link.FromNode) << "\" -> \"" << EscapeDot(link.ToNode) << "\" [style=dotted, label=\""
               << EscapeDot(link.Label) << "\"];\n";
    output << "}\n";
    if (!output) throw std::runtime_error("Failed to write DAG DOT file: " + filename);
}

std::vector<std::string> DAGManager::GetNodeNames() const
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    std::vector<std::string> names;
    names.reserve(m_Nodes.size());
    for (const auto &[name, _] : m_Nodes)
        names.push_back(name);
    return names;
}

std::vector<DAGNodeResult> DAGManager::GetNodeResults() const
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    std::vector<DAGNodeResult> results;
    results.reserve(m_Nodes.size());
    for (const auto &[name, node] : m_Nodes)
        results.push_back({name, node.Status, node.Message});
    return results;
}

bool DAGManager::IsExecuting() const
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    return m_Executing;
}
