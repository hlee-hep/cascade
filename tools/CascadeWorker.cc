#include "AMCM.hh"
#include "IsolatedWorker.hh"

#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <unistd.h>

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
