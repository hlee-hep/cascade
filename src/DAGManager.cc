#include "DAGManager.hh"
#include "ExecutionResources.hh"
#include "RuntimeOptions.hh"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <memory>
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

std::size_t DagWorkerCount(const RuntimeOptions &options)
{
    const auto configured = options.DagWorkers;
    if (configured > 0) return configured;
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
                        std::function<void()> task;
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

    void Submit(std::function<void()> task)
    {
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_Tasks.push_back(std::move(task));
        }
        m_Ready.notify_one();
    }

  private:
    std::mutex m_Mutex;
    std::condition_variable m_Ready;
    std::deque<std::function<void()>> m_Tasks;
    std::vector<std::thread> m_Threads;
    bool m_Stopping = false;
};
} // namespace

DAGManager::DAGManager() : DAGManager(GetRuntimeOptions()) {}

DAGManager::DAGManager(RuntimeOptions options) : m_RuntimeOptions(std::move(options)) {}

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
    std::unordered_map<std::string, std::size_t> nodeIndex;
    std::vector<std::vector<std::size_t>> dependents;
    std::vector<std::vector<std::pair<std::string, DataTransfer>>> incomingTransfers;
    std::vector<std::size_t> remainingDependencies;
    const RuntimeOptions runtimeOptions = m_RuntimeOptions;
    const std::size_t maxWorkers = DagWorkerCount(runtimeOptions);
    {
        std::lock_guard<std::recursive_mutex> lock(m_Mutex);
        if (m_Executing) throw std::runtime_error("DAG execution is already in progress.");
        Validate_();
        order = TopologicalOrder_();
        nodeIndex.reserve(order.size());
        for (std::size_t index = 0; index < order.size(); ++index)
            nodeIndex.emplace(order[index], index);
        dependents.resize(order.size());
        incomingTransfers.resize(order.size());
        remainingDependencies.assign(order.size(), 0);
        for (std::size_t index = 0; index < order.size(); ++index)
            for (const auto &dependency : m_Nodes.at(order[index]).Dependencies)
                dependents[nodeIndex.at(dependency)].push_back(index);
        for (const auto &link : m_DataLinks)
            incomingTransfers[nodeIndex.at(link.ToNode)].emplace_back(link.Label, link.Transfer);
        m_Executing = true;
    }
    try
    {
        struct WorkItem
        {
            std::size_t Index = 0;
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
                           entry.second.Lane == DAGExecutionLane::Isolated ||
                           entry.second.Lane == DAGExecutionLane::Root;
                }));
        }
        std::unique_ptr<TaskPool> pool;
        if (pooledNodeCount > 0) pool = std::make_unique<TaskPool>(std::min(maxWorkers, pooledNodeCount));

        std::deque<std::size_t> readySerial;
        std::deque<std::size_t> readyParallel;
        std::deque<std::size_t> readyRoot;
        std::size_t pendingCount = 0;

        auto enqueueReady = [&](std::size_t index)
        {
            const auto lane = m_Nodes.at(order[index]).Lane;
            if (lane == DAGExecutionLane::Serial)
                readySerial.push_back(index);
            else if (lane == DAGExecutionLane::Root)
                readyRoot.push_back(index);
            else
                readyParallel.push_back(index);
        };

        {
            std::lock_guard<std::recursive_mutex> lock(m_Mutex);
            // Topological order lets blocked state propagate through an already
            // failed dependency in one pass.
            for (std::size_t index = 0; index < order.size(); ++index)
            {
                auto &node = m_Nodes.at(order[index]);
                if (node.Status != DAGNodeStatus::Pending) continue;
                const auto failedDependency = std::find_if(
                    node.Dependencies.begin(), node.Dependencies.end(),
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
                for (const auto &dependency : node.Dependencies)
                    if (m_Nodes.at(dependency).Status != DAGNodeStatus::Succeeded)
                        ++remainingDependencies[index];
                ++pendingCount;
                if (remainingDependencies[index] == 0) enqueueReady(index);
            }
        }

        auto prepareWork = [&](std::size_t index)
        {
            WorkItem work;
            std::lock_guard<std::recursive_mutex> lock(m_Mutex);
            const auto &name = order[index];
            auto &node = m_Nodes.at(name);
            node.Status = DAGNodeStatus::Running;
            node.Message.clear();
            work.Index = index;
            work.Name = name;
            work.Action = node.Action;
            work.Lane = node.Lane;
            work.Transfers = incomingTransfers[index];
            --pendingCount;
            return work;
        };

        auto runWork = [&](WorkItem work)
        {
            RuntimeOptionsScope runtimeScope(runtimeOptions);
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

        struct Completion
        {
            std::size_t Index = 0;
            DAGExecutionLane Lane = DAGExecutionLane::Serial;
            bool Succeeded = false;
        };
        std::mutex completionMutex;
        std::condition_variable completionReady;
        std::deque<Completion> completions;
        std::size_t active = 0;
        bool rootActive = false;
        bool stopDispatch = false;
        std::atomic<bool> failureObserved{false};

        auto dispatch = [&](WorkItem work)
        {
            const std::size_t index = work.Index;
            const DAGExecutionLane lane = work.Lane;
            ++active;
            if (lane == DAGExecutionLane::Root) rootActive = true;
            pool->Submit(
                [&, work = std::move(work), index, lane]() mutable
                {
                    const bool succeeded = runWork(std::move(work));
                    if (!succeeded) failureObserved.store(true, std::memory_order_release);
                    {
                        std::lock_guard<std::mutex> lock(completionMutex);
                        completions.push_back({index, lane, succeeded});
                    }
                    completionReady.notify_one();
                });
        };

        auto blockDescendants = [&](std::size_t failedIndex)
        {
            std::deque<std::size_t> queue{failedIndex};
            std::lock_guard<std::recursive_mutex> lock(m_Mutex);
            while (!queue.empty())
            {
                const auto source = queue.front();
                queue.pop_front();
                for (const auto child : dependents[source])
                {
                    auto &node = m_Nodes.at(order[child]);
                    if (node.Status != DAGNodeStatus::Pending) continue;
                    node.Status = DAGNodeStatus::Blocked;
                    node.Message = "Blocked by dependency: " + order[failedIndex];
                    --pendingCount;
                    queue.push_back(child);
                }
            }
        };

        auto completeSucceeded = [&](std::size_t completedIndex)
        {
            std::lock_guard<std::recursive_mutex> lock(m_Mutex);
            for (const auto child : dependents[completedIndex])
            {
                if (m_Nodes.at(order[child]).Status != DAGNodeStatus::Pending) continue;
                if (remainingDependencies[child] == 0)
                    throw std::logic_error("DAG scheduler dependency count underflow");
                --remainingDependencies[child];
                if (remainingDependencies[child] == 0) enqueueReady(child);
            }
        };

        auto processCompletion = [&](Completion completion)
        {
            --active;
            if (completion.Lane == DAGExecutionLane::Root) rootActive = false;
            if (completion.Succeeded)
                completeSucceeded(completion.Index);
            else
            {
                blockDescendants(completion.Index);
                if (failFast) stopDispatch = true;
            }
        };

        while (true)
        {
            if (failFast && failureObserved.load(std::memory_order_acquire)) stopDispatch = true;
            if (stopDispatch && active == 0) break;
            if (pendingCount == 0 && active == 0) break;

            if (!stopDispatch)
            {
                if (!readySerial.empty())
                {
                    if (active == 0)
                    {
                        const auto index = readySerial.front();
                        readySerial.pop_front();
                        const bool succeeded = runWork(prepareWork(index));
                        if (succeeded)
                            completeSucceeded(index);
                        else
                        {
                            blockDescendants(index);
                            if (failFast) stopDispatch = true;
                        }
                        continue;
                    }
                }
                else
                {
                    while (active < maxWorkers && !readyParallel.empty())
                    {
                        const auto index = readyParallel.front();
                        readyParallel.pop_front();
                        dispatch(prepareWork(index));
                    }
                    if (active < maxWorkers && !rootActive && !readyRoot.empty())
                    {
                        const auto index = readyRoot.front();
                        readyRoot.pop_front();
                        dispatch(prepareWork(index));
                    }
                }
            }

            if (active == 0)
            {
                if (pendingCount == 0 || stopDispatch) break;
                throw std::logic_error("DAG scheduler reached pending nodes without a runnable dependency set");
            }

            {
                std::unique_lock<std::mutex> lock(completionMutex);
                completionReady.wait(lock, [&]() { return !completions.empty(); });
                do
                {
                    Completion completion = std::move(completions.front());
                    completions.pop_front();
                    lock.unlock();
                    processCompletion(std::move(completion));
                    lock.lock();
                } while (!completions.empty());
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
