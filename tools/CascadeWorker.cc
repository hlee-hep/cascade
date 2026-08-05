#include "AMCM.hh"
#include "IsolatedWorker.hh"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#include <unistd.h>
#include <vector>

namespace
{
bool WriteAll(int descriptor, const void *data, std::size_t size)
{
    const auto *bytes = static_cast<const unsigned char *>(data);
    while (size > 0)
    {
        const ssize_t written = write(descriptor, bytes, size);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        bytes += written;
        size -= static_cast<std::size_t>(written);
    }
    return true;
}

bool SendResult(int descriptor, RunResult result)
{
    if (result.Message.size() > kCascadeWorkerMaxMessageSize) result.Message.resize(kCascadeWorkerMaxMessageSize);
    IsolatedRunHeader header;
    header.Status = static_cast<std::int32_t>(result.Status);
    header.Phase = static_cast<std::int32_t>(result.Phase);
    header.MessageSize = static_cast<std::uint32_t>(result.Message.size());
    return WriteAll(descriptor, &header, sizeof(header)) &&
           WriteAll(descriptor, result.Message.data(), result.Message.size());
}

RunResult Failure(const std::string &message)
{
    return {ModuleStatus::Failed, ModulePhase::Execute, message,
            std::make_exception_ptr(std::runtime_error(message))};
}

void ApplyResourceLimit(const char *variable, int resource, rlim_t scale)
{
    const char *configured = std::getenv(variable);
    if (!configured || !*configured) return;
    std::size_t parsed = 0;
    const std::string text(configured);
    if (text.front() == '-') throw std::runtime_error(std::string(variable) + " must be a positive integer");
    const unsigned long long value = std::stoull(text, &parsed);
    if (parsed != text.size() || value == 0 || value > std::numeric_limits<rlim_t>::max() / scale)
        throw std::runtime_error(std::string(variable) + " must be a positive integer");
    struct rlimit limit{};
    if (getrlimit(resource, &limit) != 0) throw std::runtime_error(std::string("Cannot read resource limit ") + variable);
    const rlim_t requested = static_cast<rlim_t>(value) * scale;
    limit.rlim_cur = limit.rlim_max == RLIM_INFINITY ? requested : std::min(requested, limit.rlim_max);
    if (setrlimit(resource, &limit) != 0) throw std::runtime_error(std::string("Cannot apply resource limit ") + variable);
}

void HardenWorkerProcess(int resultDescriptor)
{
    umask(0077);
    const int descriptorFlags = fcntl(resultDescriptor, F_GETFD, 0);
    if (descriptorFlags < 0 || fcntl(resultDescriptor, F_SETFD, descriptorFlags | FD_CLOEXEC) != 0)
        throw std::runtime_error("Cannot protect isolated worker result descriptor");
#if defined(__linux__)
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        throw std::runtime_error("Cannot enable no_new_privs for isolated worker");
#endif
    DIR *directory = opendir("/proc/self/fd");
    if (directory)
    {
        const int directoryDescriptor = dirfd(directory);
        std::vector<int> descriptors;
        while (const dirent *entry = readdir(directory))
        {
            char *end = nullptr;
            const long descriptor = std::strtol(entry->d_name, &end, 10);
            if (!end || *end != '\0' || descriptor < 3 || descriptor == resultDescriptor || descriptor == directoryDescriptor)
                continue;
            descriptors.push_back(static_cast<int>(descriptor));
        }
        closedir(directory);
        for (const int descriptor : descriptors)
            close(descriptor);
    }
    ApplyResourceLimit("CASCADE_WORKER_MEMORY_LIMIT_MB", RLIMIT_AS, 1024ULL * 1024ULL);
    ApplyResourceLimit("CASCADE_WORKER_FILE_SIZE_LIMIT_MB", RLIMIT_FSIZE, 1024ULL * 1024ULL);
    ApplyResourceLimit("CASCADE_WORKER_MAX_PROCESSES", RLIMIT_NPROC, 1);
    ApplyResourceLimit("CASCADE_WORKER_MAX_OPEN_FILES", RLIMIT_NOFILE, 1);
}
} // namespace

int main(int argc, char **argv)
{
    RunResult result;
    int resultDescriptor = -1;
    try
    {
        if (argc != 2) throw std::runtime_error("Isolated worker requires a result descriptor");
        resultDescriptor = std::stoi(argv[1]);
        if (resultDescriptor < 3) throw std::runtime_error("Invalid isolated worker result descriptor");
        HardenWorkerProcess(resultDescriptor);
        nlohmann::json request;
        std::cin >> request;
        if (request.value("schema", 0) != 1) throw std::runtime_error("Unsupported isolated worker request schema");

        const bool requireSigned = request.value("require_signed", false);
        AMCM controller(requireSigned ? PluginTrustPolicy::RequireSigned : PluginTrustPolicy::Verified, false);
        controller.LoadPluginPackage(request.at("manifest_path").get<std::string>(),
                                     request.at("module").get<std::string>());
        auto module = controller.RegisterModule(request.at("module").get<std::string>(),
                                                request.at("instance").get<std::string>());
        const auto origin = module->GetPluginOrigin();
        if (!origin) throw std::runtime_error("Isolated execution requires a verified plugin origin");
        if (origin->ManifestSha256 != request.at("manifest_sha256").get<std::string>() ||
            origin->ArtifactSha256 != request.at("artifact_sha256").get<std::string>())
            throw std::runtime_error("Plugin changed between isolated execution validation and worker startup");

        module->SetCacheDirectory(request.at("cache_directory").get<std::string>());
        module->SetOutputDirectory(request.at("output_directory").get<std::string>());
        module->SetParamsFromJSON(request.at("params").dump());
        module->PrepareExternalRunWithId(request.at("run_id").get<std::string>());
        result = module->RunPreparedExternal();
    }
    catch (const std::exception &error)
    {
        result = Failure(error.what());
    }
    catch (...)
    {
        result = Failure("Unknown exception escaped isolated C++ worker");
    }
    return resultDescriptor >= 0 && SendResult(resultDescriptor, std::move(result)) ? 0 : 125;
}
