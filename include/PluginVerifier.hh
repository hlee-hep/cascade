#pragma once

#include "PluginTrust.hh"
#include <string>
#include <vector>

struct VerifiedPluginArtifact
{
    std::string Name;
    std::string Language;
    std::string Path;
    std::string Sha256;
    std::vector<std::string> Classes;
    std::string Source;
    PluginOrigin Origin;

    VerifiedPluginArtifact() = default;
    VerifiedPluginArtifact(const VerifiedPluginArtifact &other);
    VerifiedPluginArtifact &operator=(const VerifiedPluginArtifact &other);
    VerifiedPluginArtifact(VerifiedPluginArtifact &&other) noexcept;
    VerifiedPluginArtifact &operator=(VerifiedPluginArtifact &&other) noexcept;
    ~VerifiedPluginArtifact();

    int Descriptor() const { return m_Descriptor; }

  private:
    friend class PluginVerifier;
    int m_Descriptor = -1;
};

struct VerifiedPluginPackage
{
    std::string Package;
    std::string ManifestPath;
    std::string ManifestSha256;
    std::string TrustedKeyPath;
    std::string SignerFingerprint;
    PluginTrustStatus Trust = PluginTrustStatus::Verified;
    std::vector<VerifiedPluginArtifact> Artifacts;
};

struct PluginDiscoveryResult
{
    std::vector<VerifiedPluginPackage> Packages;
    std::vector<std::string> Errors;
};

class PluginVerifier
{
  public:
    // Produces a metadata fingerprint for plugin roots and trust stores. Runtimes
    // can invalidate discovery caches without duplicating filesystem traversal.
    static std::string IndexFingerprint(const std::vector<std::string> &pluginRoots,
                                        const std::vector<std::string> &trustStores);
    static PluginDiscoveryResult Discover(const std::vector<std::string> &pluginRoots,
                                          PluginTrustPolicy policy = PluginTrustPolicy::Verified,
                                          const std::string &language = "");
    static std::string HashFile(const std::string &path);
    static std::string HashBytes(const std::string &data);
    static std::string ReadFile(const std::string &path);
    static void ValidateDirectoryTree(const std::string &path, bool allowMissingLeaf = false);
    static void ValidateStagedTree(const std::string &packageDirectory);

    // Verifies one immutable package snapshot. Any invalid manifest entry rejects
    // the entire package instead of leaving runtimes to interpret it differently.
    static VerifiedPluginPackage VerifyPackage(const std::string &packageDirectory,
                                               const std::string &trustStore,
                                               PluginTrustPolicy policy = PluginTrustPolicy::Verified,
                                               const std::string &language = "",
                                               const std::string &trustedKey = "",
                                               const std::string &moduleIdentity = "");
};
