#include "DAGManager.hh"
#include "RuntimeOptions.hh"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;

struct Options
{
    std::vector<std::size_t> Nodes{1, 10, 100, 1000};
    std::size_t Iterations = 3;
    std::size_t WorkMilliseconds = 0;
    std::vector<std::size_t> Workers{1, 2, 4};
    bool Json = false;
};

struct Measurement
{
    std::string Scenario;
    std::string Limiter;
    std::size_t Nodes = 1;
    std::size_t Workers = 1;
    double MedianMilliseconds = 0.0;
    double P95Milliseconds = 0.0;
    double Throughput = 0.0;
    double LowerBoundMilliseconds = 0.0;
    double OverheadRatio = 0.0;
};

std::size_t ParsePositive(const std::string &value, const std::string &flag, bool allowZero = false)
{
    if (value.empty() || value.front() == '-')
        throw std::invalid_argument(flag + " requires " + (allowZero ? "a non-negative" : "a positive") + " integer");
    std::size_t consumed = 0;
    const unsigned long long parsed = std::stoull(value, &consumed);
    if (consumed != value.size() || (!allowZero && parsed == 0) || parsed > std::numeric_limits<std::size_t>::max())
        throw std::invalid_argument(flag + " requires " + (allowZero ? "a non-negative" : "a positive") + " integer");
    return static_cast<std::size_t>(parsed);
}

std::vector<std::size_t> ParseList(const std::string &value, const std::string &flag)
{
    if (value.empty() || value.back() == ',') throw std::invalid_argument(flag + " requires a comma-separated list");
    std::vector<std::size_t> values;
    std::size_t begin = 0;
    while (begin < value.size())
    {
        const auto end = value.find(',', begin);
        values.push_back(ParsePositive(value.substr(begin, end - begin), flag));
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    if (values.empty()) throw std::invalid_argument(flag + " requires a comma-separated list");
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}

Options ParseOptions(int argc, char **argv)
{
    Options options;
    for (int index = 1; index < argc; ++index)
    {
        const std::string argument(argv[index]);
        auto next = [&](const std::string &flag)
        {
            if (++index >= argc) throw std::invalid_argument(flag + " requires a value");
            return std::string(argv[index]);
        };
        if (argument == "--nodes")
            options.Nodes = ParseList(next(argument), argument);
        else if (argument == "--iterations")
            options.Iterations = ParsePositive(next(argument), argument);
        else if (argument == "--work-ms")
            options.WorkMilliseconds = ParsePositive(next(argument), argument, true);
        else if (argument == "--workers")
            options.Workers = ParseList(next(argument), argument);
        else if (argument == "--json")
            options.Json = true;
        else if (argument == "--help")
        {
            std::cout << "Usage: cascade-dag-bench [--nodes 1,10,100,1000] [--iterations N] [--work-ms N] "
                         "[--workers 1,2,4] [--json]\n";
            std::exit(0);
        }
        else
            throw std::invalid_argument("unknown argument: " + argument);
    }
    return options;
}

double Percentile(std::vector<double> samples, double percentile)
{
    std::sort(samples.begin(), samples.end());
    const auto index = static_cast<std::size_t>(std::ceil(percentile * samples.size())) - 1;
    return samples.at(std::min(index, samples.size() - 1));
}

Measurement RunScenario(const Options &options, const std::string &scenario, const std::string &limiter,
                        DAGExecutionLane lane, bool chain, std::size_t nodes, std::size_t workers)
{
    RuntimeOptions runtime = GetRuntimeOptions();
    runtime.DagWorkers = workers;
    SetRuntimeOptions(runtime);

    DAGManager dag;
    for (std::size_t index = 0; index < nodes; ++index)
    {
        std::vector<std::string> dependencies;
        if (chain && index > 0) dependencies.push_back("node-" + std::to_string(index - 1));
        dag.AddNode(
            "node-" + std::to_string(index), dependencies,
            [&options]()
            {
                if (options.WorkMilliseconds > 0)
                    std::this_thread::sleep_for(std::chrono::milliseconds(options.WorkMilliseconds));
            },
            lane);
    }

    // The warm-up removes one-time loader and allocator noise from the samples.
    if (!dag.Execute().Succeeded()) throw std::runtime_error("benchmark warm-up failed");
    std::vector<double> samples;
    samples.reserve(options.Iterations);
    for (std::size_t iteration = 0; iteration < options.Iterations; ++iteration)
    {
        dag.Reset();
        const auto started = Clock::now();
        const auto result = dag.Execute();
        const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        if (!result.Succeeded()) throw std::runtime_error("benchmark execution failed");
        samples.push_back(elapsed);
    }

    const double median = Percentile(samples, 0.5);
    double lowerBound = 0.0;
    if (options.WorkMilliseconds > 0)
    {
        const bool forcedSerial = lane == DAGExecutionLane::Serial || lane == DAGExecutionLane::Root || chain;
        const std::size_t waves = forcedSerial ? nodes : (nodes + workers - 1) / workers;
        lowerBound = static_cast<double>(waves * options.WorkMilliseconds);
    }
    return {
        scenario,
        limiter,
        nodes,
        workers,
        median,
        Percentile(samples, 0.95),
        median == 0.0 ? 0.0 : static_cast<double>(nodes) * 1000.0 / median,
        lowerBound,
        lowerBound == 0.0 ? 0.0 : median / lowerBound,
    };
}

void PrintJson(const Options &options, const std::vector<Measurement> &measurements)
{
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "{\n  \"node_counts\": [";
    for (std::size_t index = 0; index < options.Nodes.size(); ++index)
        std::cout << (index == 0 ? "" : ", ") << options.Nodes[index];
    std::cout << "],\n  \"iterations\": " << options.Iterations << ",\n  \"work_ms\": " << options.WorkMilliseconds
              << ",\n  \"measurements\": [\n";
    for (std::size_t index = 0; index < measurements.size(); ++index)
    {
        const auto &value = measurements[index];
        std::cout << "    {\"scenario\": \"" << value.Scenario << "\", \"limiter\": \"" << value.Limiter
                  << "\", \"nodes\": " << value.Nodes << ", \"workers\": " << value.Workers
                  << ", \"median_ms\": " << value.MedianMilliseconds
                  << ", \"p95_ms\": " << value.P95Milliseconds << ", \"nodes_per_second\": " << value.Throughput
                  << ", \"lower_bound_ms\": " << value.LowerBoundMilliseconds
                  << ", \"overhead_ratio\": " << value.OverheadRatio << "}";
        std::cout << (index + 1 == measurements.size() ? "\n" : ",\n");
    }
    std::cout << "  ]\n}\n";
}

void PrintTable(const Options &options, const std::vector<Measurement> &measurements)
{
    std::cout << "Cascade DAG bottleneck testbench\n"
              << "iterations=" << options.Iterations << " work_ms=" << options.WorkMilliseconds << "\n\n";
    std::cout << std::left << std::setw(12) << "scenario" << std::setw(18) << "expected limiter" << std::right
              << std::setw(8) << "nodes" << std::setw(9) << "workers" << std::setw(13) << "median ms" << std::setw(11) << "p95 ms"
              << std::setw(15) << "nodes/sec" << std::setw(12) << "overhead" << '\n';
    for (const auto &value : measurements)
    {
        std::cout << std::left << std::setw(12) << value.Scenario << std::setw(18) << value.Limiter << std::right
                  << std::setw(8) << value.Nodes << std::setw(9) << value.Workers << std::setw(13) << std::fixed << std::setprecision(3)
                  << value.MedianMilliseconds << std::setw(11) << value.P95Milliseconds << std::setw(15)
                  << value.Throughput << std::setw(11);
        if (value.LowerBoundMilliseconds == 0.0)
            std::cout << "n/a";
        else
            std::cout << value.OverheadRatio << 'x';
        std::cout << '\n';
    }
    std::cout << "\noverhead = median / topology lower bound; values near 1.0x are workload-limited.\n";
}
} // namespace

int main(int argc, char **argv)
{
    try
    {
        const Options options = ParseOptions(argc, argv);
        std::vector<Measurement> measurements;
        for (const auto nodes : options.Nodes)
        {
            for (const auto workers : options.Workers)
            {
                measurements.push_back(RunScenario(options, "serial", "serial lane", DAGExecutionLane::Serial, false, nodes, workers));
                measurements.push_back(RunScenario(options, "parallel", "worker pool", DAGExecutionLane::Parallel, false, nodes, workers));
                measurements.push_back(RunScenario(options, "root", "ROOT mutex", DAGExecutionLane::Root, false, nodes, workers));
                measurements.push_back(RunScenario(options, "chain", "critical path", DAGExecutionLane::Parallel, true, nodes, workers));
            }
        }
        if (options.Json)
            PrintJson(options, measurements);
        else
            PrintTable(options, measurements);
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "cascade-dag-bench: " << error.what() << '\n';
        return 2;
    }
}
