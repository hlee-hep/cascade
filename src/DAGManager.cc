#include "DAGManager.hh"
#include "ExecutionResources.hh"

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <thread>
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

std::size_t DagWorkerCount()
{
    const char *configured = std::getenv("CASCADE_DAG_MAX_WORKERS");
    if (configured && *configured)
    {
        if (*configured == '-') throw std::runtime_error("CASCADE_DAG_MAX_WORKERS must be a positive integer");
        char *end = nullptr;
        errno = 0;
        const unsigned long value = std::strtoul(configured, &end, 10);
        if (errno != 0 || end == configured || *end != '\0' || value == 0)
            throw std::runtime_error("CASCADE_DAG_MAX_WORKERS must be a positive integer");
        return static_cast<std::size_t>(value);
    }
    const unsigned int detected = std::thread::hardware_concurrency();
    return detected == 0 ? 1 : static_cast<std::size_t>(detected);
}

class TaskPool
{
  public:
    explicit TaskPool(std::size_t size)
    {
        m_Threads.reserve(size);
        for (std::size_t index = 0; index < size; ++index)
            m_Threads.emplace_back(
                [this]()
                {
                    while (true)
                    {
                        std::packaged_task<bool()> task;
                        {
                            std::unique_lock<std::mutex> lock(m_Mutex);
                            m_Ready.wait(lock, [&]() { return m_Stopping || !m_Tasks.empty(); });
                            if (m_Stopping && m_Tasks.empty()) return;
                            task = std::move(m_Tasks.front());
                            m_Tasks.pop_front();
                        }
                        task();
                    }
                });
    }

    ~TaskPool()
    {
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_Stopping = true;
        }
        m_Ready.notify_all();
        for (auto &thread : m_Threads)
            thread.join();
    }

    TaskPool(const TaskPool &) = delete;
    TaskPool &operator=(const TaskPool &) = delete;

    std::future<bool> Submit(std::packaged_task<bool()> task)
    {
        auto result = task.get_future();
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_Tasks.push_back(std::move(task));
        }
        m_Ready.notify_one();
        return result;
    }

  private:
    std::mutex m_Mutex;
    std::condition_variable m_Ready;
    std::deque<std::packaged_task<bool()>> m_Tasks;
    std::vector<std::thread> m_Threads;
    bool m_Stopping = false;
};
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

void DAGManager::AddNode(const std::string &name, const std::vector<std::string> &dependencies, Task task,
                         DAGExecutionLane lane)
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
    m_Nodes.emplace(name, Node{name, dependencies, std::move(task), lane});
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

void DAGManager::Validate() const
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    Validate_();
}

DAGRunResult DAGManager::Execute(bool failFast)
{
    std::vector<std::string> order;
    const std::size_t maxWorkers = DagWorkerCount();
    {
        std::lock_guard<std::recursive_mutex> lock(m_Mutex);
        if (m_Executing) throw std::runtime_error("DAG execution is already in progress.");
        Validate_();
        order = TopologicalOrder_();
        m_Executing = true;
    }
    try
    {
        struct WorkItem
        {
            std::string Name;
            Task Action;
            std::vector<std::pair<std::string, DataTransfer>> Transfers;
            DAGExecutionLane Lane = DAGExecutionLane::Serial;
        };

        std::size_t pooledNodeCount = 0;
        {
            std::lock_guard<std::recursive_mutex> lock(m_Mutex);
            pooledNodeCount = static_cast<std::size_t>(std::count_if(
                m_Nodes.begin(), m_Nodes.end(), [](const auto &entry)
                {
                    return entry.second.Lane == DAGExecutionLane::Parallel ||
                           entry.second.Lane == DAGExecutionLane::Isolated;
                }));
        }
        std::unique_ptr<TaskPool> pool;
        if (pooledNodeCount > 0) pool = std::make_unique<TaskPool>(std::min(maxWorkers, pooledNodeCount));

        auto prepareWork = [&](const std::string &name)
        {
            WorkItem work;
            std::lock_guard<std::recursive_mutex> lock(m_Mutex);
            auto &node = m_Nodes.at(name);
            node.Status = DAGNodeStatus::Running;
            node.Message.clear();
            work.Name = name;
            work.Action = node.Action;
            work.Lane = node.Lane;
            for (const auto &link : m_DataLinks)
                if (link.ToNode == name) work.Transfers.emplace_back(link.Label, link.Transfer);
            return work;
        };

        auto runWork = [&](WorkItem work)
        {
            std::unique_lock<std::recursive_mutex> rootLock(CascadeRootExecutionMutex(), std::defer_lock);
            if (work.Lane == DAGExecutionLane::Root) rootLock.lock();
            try
            {
                for (const auto &[label, transfer] : work.Transfers)
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
                work.Action();
                std::lock_guard<std::recursive_mutex> lock(m_Mutex);
                m_Nodes.at(work.Name).Status = DAGNodeStatus::Succeeded;
                return true;
            }
            catch (const std::exception &error)
            {
                std::lock_guard<std::recursive_mutex> lock(m_Mutex);
                auto &node = m_Nodes.at(work.Name);
                node.Status = DAGNodeStatus::Failed;
                node.Message = error.what();
                return false;
            }
            catch (...)
            {
                std::lock_guard<std::recursive_mutex> lock(m_Mutex);
                auto &node = m_Nodes.at(work.Name);
                node.Status = DAGNodeStatus::Failed;
                node.Message = "Unknown task exception";
                return false;
            }
        };

        while (true)
        {
            std::vector<std::string> ready;
            bool pending = false;
            {
                std::lock_guard<std::recursive_mutex> lock(m_Mutex);
                for (const auto &name : order)
                {
                    auto &node = m_Nodes.at(name);
                    if (node.Status != DAGNodeStatus::Pending) continue;
                    pending = true;
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
                    const bool dependenciesComplete =
                        std::all_of(node.Dependencies.begin(), node.Dependencies.end(),
                                    [&](const std::string &dependency)
                                    { return m_Nodes.at(dependency).Status == DAGNodeStatus::Succeeded; });
                    if (dependenciesComplete) ready.push_back(name);
                }
            }
            if (!pending) break;
            if (ready.empty())
            {
                bool stillPending = false;
                {
                    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
                    stillPending = std::any_of(m_Nodes.begin(), m_Nodes.end(), [](const auto &entry)
                                               { return entry.second.Status == DAGNodeStatus::Pending; });
                }
                if (!stillPending) break;
                throw std::logic_error("DAG scheduler reached pending nodes without a runnable dependency set");
            }

            const auto serial = std::find_if(ready.begin(), ready.end(), [&](const std::string &name)
                                             {
                                                 std::lock_guard<std::recursive_mutex> lock(m_Mutex);
                                                 return m_Nodes.at(name).Lane == DAGExecutionLane::Serial;
                                             });
            if (serial != ready.end())
            {
                const bool succeeded = runWork(prepareWork(*serial));
                if (failFast && !succeeded)
                {
                    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
                    MarkBlockedDescendants_(*serial);
                    break;
                }
                continue;
            }

            std::vector<WorkItem> batch;
            bool rootSelected = false;
            for (const auto &name : ready)
            {
                DAGExecutionLane lane;
                {
                    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
                    lane = m_Nodes.at(name).Lane;
                }
                if (lane == DAGExecutionLane::Root && rootSelected) continue;
                if (batch.size() >= maxWorkers) break;
                if (lane == DAGExecutionLane::Root) rootSelected = true;
                batch.push_back(prepareWork(name));
            }

            std::vector<std::pair<std::string, std::future<bool>>> futures;
            std::optional<WorkItem> rootWork;
            for (auto &work : batch)
            {
                if (work.Lane == DAGExecutionLane::Root)
                    rootWork = std::move(work);
                else
                {
                    const std::string workName = work.Name;
                    std::packaged_task<bool()> task(
                        [&, work = std::move(work)]() mutable { return runWork(std::move(work)); });
                    futures.emplace_back(workName, pool->Submit(std::move(task)));
                }
            }
            std::vector<std::string> failures;
            if (rootWork)
            {
                const std::string rootName = rootWork->Name;
                if (!runWork(std::move(*rootWork))) failures.push_back(rootName);
            }
            for (auto &[name, future] : futures)
                if (!future.get()) failures.push_back(name);
            if (failFast && !failures.empty())
            {
                std::lock_guard<std::recursive_mutex> lock(m_Mutex);
                for (const auto &name : failures) MarkBlockedDescendants_(name);
                break;
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

std::map<std::string, std::vector<std::string>> DAGManager::GetDependencies() const
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    std::map<std::string, std::vector<std::string>> dependencies;
    for (const auto &[name, node] : m_Nodes)
        dependencies[name] = node.Dependencies;
    return dependencies;
}

std::vector<DAGDataLinkInfo> DAGManager::GetDataLinks() const
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    std::vector<DAGDataLinkInfo> links;
    links.reserve(m_DataLinks.size());
    for (const auto &link : m_DataLinks)
        links.push_back({link.FromNode, link.ToNode, link.Label});
    return links;
}

bool DAGManager::IsExecuting() const
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    return m_Executing;
}
