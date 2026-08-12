#include "Provenance.hh"
#include "RuntimeOptions.hh"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

namespace
{
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

struct Options
{
    fs::path Input;
    std::size_t FixtureMiB = 128;
    std::size_t Iterations = 5;
    bool Json = false;
};

struct Measurement
{
    std::string RequestedMode;
    std::string ResolvedMode;
    double FirstMilliseconds = 0.0;
    double WarmMedianMilliseconds = 0.0;
    double FirstMiBPerSecond = 0.0;
    double WarmMiBPerSecond = 0.0;
};

std::size_t ParsePositive(const std::string &value, const std::string &flag)
{
    if (value.empty() || value.front() == '-') throw std::invalid_argument(flag + " requires a positive integer");
    std::size_t consumed = 0;
    const auto parsed = std::stoull(value, &consumed);
    if (consumed != value.size() || parsed == 0) throw std::invalid_argument(flag + " requires a positive integer");
    return static_cast<std::size_t>(parsed);
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
        if (argument == "--input")
            options.Input = next(argument);
        else if (argument == "--fixture-mib")
            options.FixtureMiB = ParsePositive(next(argument), argument);
        else if (argument == "--iterations")
            options.Iterations = ParsePositive(next(argument), argument);
        else if (argument == "--json")
            options.Json = true;
        else if (argument == "--help")
        {
            std::cout << "Usage: cascade-input-hash-bench [--input FILE] [--fixture-mib N] [--iterations N] [--json]\n";
            std::exit(0);
        }
        else
            throw std::invalid_argument("unknown argument: " + argument);
    }
    if (options.Iterations < 2) throw std::invalid_argument("--iterations must be at least 2");
    return options;
}

class TemporaryFixture
{
  public:
    explicit TemporaryFixture(std::size_t sizeMiB)
    {
        char pathTemplate[] = "/tmp/cascade-hash-bench-XXXXXX.root";
        const int descriptor = mkstemps(pathTemplate, 5);
        if (descriptor < 0) throw std::runtime_error("cannot allocate a temporary hash benchmark fixture");
        close(descriptor);
        Path = pathTemplate;
        std::ofstream output(Path, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            fs::remove(Path);
            throw std::runtime_error("cannot create hash benchmark fixture: " + Path.string());
        }
        std::vector<char> block(1024 * 1024);
        for (std::size_t index = 0; index < block.size(); ++index)
            block[index] = static_cast<char>((index * 131U + 17U) & 0xffU);
        for (std::size_t index = 0; index < sizeMiB; ++index)
            output.write(block.data(), static_cast<std::streamsize>(block.size()));
        if (!output)
        {
            output.close();
            fs::remove(Path);
            throw std::runtime_error("cannot write hash benchmark fixture: " + Path.string());
        }
    }

    ~TemporaryFixture()
    {
        std::error_code error;
        fs::remove(Path, error);
    }

    fs::path Path;
};

double Median(std::vector<double> samples)
{
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

std::string JsonEscape(const std::string &value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (const char character : value)
    {
        switch (character)
        {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            escaped.push_back(character);
        }
    }
    return escaped;
}

Measurement Measure(const fs::path &input, std::uintmax_t bytes, const std::string &mode, std::size_t iterations)
{
    RuntimeOptions runtime = GetRuntimeOptions();
    runtime.InputHash = mode;
    SetRuntimeOptions(runtime);
    const std::string runId = "hash-benchmark-" + mode;
    ProvenanceRecorder::BeginModuleRun(runId, runId, "HashBenchmark", "cpp", false);
    ProvenanceRecorder::TrackInput(runId, input);

    std::vector<double> samples;
    samples.reserve(iterations);
    std::string state;
    for (std::size_t iteration = 0; iteration < iterations; ++iteration)
    {
        const auto started = Clock::now();
        state = ProvenanceRecorder::InputSnapshotState(runId);
        samples.push_back(std::chrono::duration<double, std::milli>(Clock::now() - started).count());
    }
    ProvenanceRecorder::DiscardModuleRun(runId);

    const std::string resolved = state.find("\"hash_mode\":\"full\"") != std::string::npos ? "full" : "metadata";
    const double first = samples.front();
    samples.erase(samples.begin());
    const double warm = Median(samples);
    const double sizeMiB = static_cast<double>(bytes) / (1024.0 * 1024.0);
    const auto throughput = [&](double milliseconds)
    { return resolved == "full" && milliseconds > 0.0 ? sizeMiB * 1000.0 / milliseconds : 0.0; };
    return {mode, resolved, first, warm, throughput(first), throughput(warm)};
}

void PrintJson(const fs::path &input, std::uintmax_t bytes, const std::vector<Measurement> &measurements)
{
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "{\n  \"input\": \"" << JsonEscape(input.string()) << "\",\n  \"bytes\": " << bytes
              << ",\n  \"measurements\": [\n";
    for (std::size_t index = 0; index < measurements.size(); ++index)
    {
        const auto &value = measurements[index];
        std::cout << "    {\"requested_mode\": \"" << value.RequestedMode << "\", \"resolved_mode\": \""
                  << value.ResolvedMode << "\", \"first_ms\": " << value.FirstMilliseconds
                  << ", \"warm_median_ms\": " << value.WarmMedianMilliseconds << ", \"first_mib_per_second\": "
                  << value.FirstMiBPerSecond << ", \"warm_mib_per_second\": " << value.WarmMiBPerSecond << "}"
                  << (index + 1 == measurements.size() ? "\n" : ",\n");
    }
    std::cout << "  ]\n}\n";
}

void PrintTable(const fs::path &input, std::uintmax_t bytes, const std::vector<Measurement> &measurements)
{
    std::cout << "Cascade input hashing bottleneck testbench\n"
              << "input=" << input << " size_mib=" << std::fixed << std::setprecision(1)
              << static_cast<double>(bytes) / (1024.0 * 1024.0) << " digest_cache=disabled\n\n";
    std::cout << std::left << std::setw(12) << "requested" << std::setw(12) << "resolved" << std::right
              << std::setw(13) << "first ms" << std::setw(16) << "warm median ms" << std::setw(16) << "warm MiB/s" << '\n';
    for (const auto &value : measurements)
    {
        std::cout << std::left << std::setw(12) << value.RequestedMode << std::setw(12) << value.ResolvedMode << std::right
                  << std::setw(13) << std::setprecision(3) << value.FirstMilliseconds << std::setw(16)
                  << value.WarmMedianMilliseconds << std::setw(16);
        if (value.ResolvedMode == "full")
            std::cout << value.WarmMiBPerSecond;
        else
            std::cout << "n/a";
        std::cout << '\n';
    }
}
} // namespace

int main(int argc, char **argv)
{
    try
    {
        const Options options = ParseOptions(argc, argv);
        // Measure the actual hashing path on every iteration instead of the
        // process-local identity-to-digest memoization fast path.
        RuntimeOptions runtime = GetRuntimeOptions();
        runtime.ArtifactHashCacheEntries = 0;
        SetRuntimeOptions(runtime);

        std::unique_ptr<TemporaryFixture> fixture;
        fs::path input = options.Input;
        if (input.empty())
        {
            fixture = std::make_unique<TemporaryFixture>(options.FixtureMiB);
            input = fixture->Path;
        }
        std::error_code error;
        if (!fs::is_regular_file(input, error) || error)
            throw std::invalid_argument("--input must name a readable regular file: " + input.string());
        const auto bytes = fs::file_size(input);

        std::vector<Measurement> measurements;
        for (const std::string mode : {"metadata", "auto", "full"})
            measurements.push_back(Measure(input, bytes, mode, options.Iterations));
        if (options.Json)
            PrintJson(input, bytes, measurements);
        else
            PrintTable(input, bytes, measurements);
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "cascade-input-hash-bench: " << error.what() << '\n';
        return 2;
    }
}
