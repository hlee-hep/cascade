#include "PluginVerifier.hh"
#include "PluginPaths.hh"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
namespace fs = std::filesystem;

class PackageReadLock
{
  public:
    explicit PackageReadLock(const fs::path &packageDir)
    {
        fs::path prefix = packageDir;
        for (int level = 0; level < 4 && prefix.has_parent_path(); ++level) prefix = prefix.parent_path();
        const fs::path lockPath = prefix / ".cascade-locks" / (packageDir.filename().string() + ".lock");
        int flags = O_RDONLY;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
        m_Descriptor = open(lockPath.string().c_str(), flags);
        if (m_Descriptor < 0)
        {
            if (errno == ENOENT) return;
            throw std::runtime_error("cannot safely open plugin package lock " + lockPath.string());
        }
        struct stat metadata{};
        if (fstat(m_Descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode))
        {
            close(m_Descriptor);
            m_Descriptor = -1;
            throw std::runtime_error("plugin package lock is not a regular file: " + lockPath.string());
        }
        while (flock(m_Descriptor, LOCK_SH) != 0)
        {
            if (errno == EINTR) continue;
            close(m_Descriptor);
            m_Descriptor = -1;
            throw std::runtime_error("cannot acquire plugin package lock " + lockPath.string());
        }
    }

    ~PackageReadLock()
    {
        if (m_Descriptor >= 0)
        {
            flock(m_Descriptor, LOCK_UN);
            close(m_Descriptor);
        }
    }

  private:
    int m_Descriptor = -1;
};

class IndexReadLock
{
  public:
    explicit IndexReadLock(const fs::path &pluginRoot)
    {
        fs::path prefix = pluginRoot;
        for (int level = 0; level < 3 && prefix.has_parent_path(); ++level) prefix = prefix.parent_path();
        const fs::path lockPath = prefix / ".cascade-locks" / ".index.lock";
        int flags = O_RDONLY;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
        m_Descriptor = open(lockPath.string().c_str(), flags);
        if (m_Descriptor < 0)
        {
            if (errno == ENOENT) return;
            throw std::runtime_error("cannot safely open plugin index lock " + lockPath.string());
        }
        struct stat metadata{};
        if (fstat(m_Descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode))
        {
            close(m_Descriptor);
            m_Descriptor = -1;
            throw std::runtime_error("plugin index lock is not a regular file " + lockPath.string());
        }
        while (flock(m_Descriptor, LOCK_SH) != 0)
        {
            if (errno == EINTR) continue;
            close(m_Descriptor);
            m_Descriptor = -1;
            throw std::runtime_error("cannot acquire plugin index lock " + lockPath.string());
        }
    }

    ~IndexReadLock()
    {
        if (m_Descriptor >= 0)
        {
            flock(m_Descriptor, LOCK_UN);
            close(m_Descriptor);
        }
    }

  private:
    int m_Descriptor = -1;
};

int OpenRegularFile(const fs::path &path)
{
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int descriptor = open(path.string().c_str(), flags);
    if (descriptor < 0)
        throw std::runtime_error("cannot open regular file " + path.string() + ": " + std::strerror(errno));
    struct stat metadata{};
    if (fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode))
    {
        close(descriptor);
        throw std::runtime_error("plugin file is not a regular file: " + path.string());
    }
    return descriptor;
}

std::vector<unsigned char> ReadDescriptor(int descriptor)
{
    if (lseek(descriptor, 0, SEEK_SET) < 0) throw std::runtime_error("cannot seek plugin file descriptor");
    std::vector<unsigned char> data;
    std::array<unsigned char, 1024 * 1024> buffer{};
    while (true)
    {
        const ssize_t count = read(descriptor, buffer.data(), buffer.size());
        if (count < 0)
        {
            if (errno == EINTR) continue;
            throw std::runtime_error("cannot read plugin file descriptor");
        }
        if (count == 0) break;
        data.insert(data.end(), buffer.begin(), buffer.begin() + count);
    }
    return data;
}

std::string Sha256Descriptor(int descriptor)
{
    if (lseek(descriptor, 0, SEEK_SET) < 0) throw std::runtime_error("cannot seek plugin file descriptor");
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    if (!context || EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1)
    {
        if (context) EVP_MD_CTX_free(context);
        throw std::runtime_error("cannot initialize plugin SHA-256 digest");
    }
    std::array<unsigned char, 1024 * 1024> buffer{};
    while (true)
    {
        const ssize_t count = read(descriptor, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0)
        {
            EVP_MD_CTX_free(context);
            throw std::runtime_error("cannot read plugin file descriptor while hashing");
        }
        if (count == 0) break;
        if (EVP_DigestUpdate(context, buffer.data(), static_cast<std::size_t>(count)) != 1)
        {
            EVP_MD_CTX_free(context);
            throw std::runtime_error("cannot update plugin SHA-256 digest");
        }
    }
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (EVP_DigestFinal_ex(context, digest, &length) != 1)
    {
        EVP_MD_CTX_free(context);
        throw std::runtime_error("cannot finalize plugin SHA-256 digest");
    }
    EVP_MD_CTX_free(context);
    if (lseek(descriptor, 0, SEEK_SET) < 0) throw std::runtime_error("cannot rewind plugin file descriptor");
    std::ostringstream out;
    for (unsigned int index = 0; index < length; ++index)
        out << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[index]);
    return out.str();
}

std::vector<unsigned char> ReadRegularFile(const fs::path &path)
{
    const int descriptor = OpenRegularFile(path);
    try
    {
        auto data = ReadDescriptor(descriptor);
        close(descriptor);
        return data;
    }
    catch (...)
    {
        close(descriptor);
        throw;
    }
}

std::string Sha256(const std::vector<unsigned char> &data)
{
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(data.data(), data.size(), hash);
    std::ostringstream out;
    for (const unsigned char value : hash)
        out << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(value);
    return out.str();
}

std::string Sha256(const std::string &data)
{
    return Sha256(std::vector<unsigned char>(data.begin(), data.end()));
}

void AppendTrackedFiles(std::ostringstream &snapshot, const fs::path &root, bool trustStore)
{
    std::error_code error;
    if (!fs::is_directory(root, error) || error)
    {
        snapshot << "missing:" << fs::absolute(root, error).lexically_normal().string() << '\n';
        return;
    }

    std::vector<fs::path> paths;
    fs::recursive_directory_iterator iterator(root, fs::directory_options::skip_permission_denied, error);
    const fs::recursive_directory_iterator end;
    while (!error && iterator != end)
    {
        const fs::directory_entry entry = *iterator;
        if (entry.is_symlink(error))
        {
            if (entry.is_directory(error)) iterator.disable_recursion_pending();
        }
        else if (entry.is_regular_file(error))
        {
            const std::string name = entry.path().filename().string();
            const bool tracked = trustStore ? entry.path().extension() == ".pem"
                                            : name == "plugin_manifest.json" || name == "plugin_manifest.json.sig" ||
                                                  entry.path().extension() == ".py" || entry.path().extension() == ".so";
            if (tracked) paths.push_back(entry.path());
        }
        error.clear();
        iterator.increment(error);
    }

    std::sort(paths.begin(), paths.end());
    for (const fs::path &path : paths)
    {
        error.clear();
        const auto size = fs::file_size(path, error);
        if (error) continue;
        const auto modified = fs::last_write_time(path, error);
        if (error) continue;
        snapshot << fs::absolute(path).lexically_normal().string() << ':' << size << ':'
                 << modified.time_since_epoch().count() << '\n';
    }
}

bool IsContainedPath(const fs::path &root, const fs::path &candidate)
{
    const fs::path canonicalRoot = fs::canonical(root);
    const fs::path canonicalCandidate = fs::canonical(candidate);
    const fs::path relative = canonicalCandidate.lexically_relative(canonicalRoot);
    return !relative.empty() && *relative.begin() != "..";
}

bool HasSymlinkComponent(const fs::path &root, const fs::path &candidate)
{
    std::error_code error;
    if (fs::is_symlink(fs::symlink_status(root, error)) || error) return true;
    fs::path current = root;
    const fs::path relative = candidate.lexically_relative(root);
    if (relative.empty() || *relative.begin() == "..") return true;
    for (const auto &component : relative)
    {
        current /= component;
        error.clear();
        if (fs::is_symlink(fs::symlink_status(current, error)) || error) return true;
    }
    return false;
}

EVP_PKEY *ReadPublicKey(const std::vector<unsigned char> &data)
{
    BIO *bio = BIO_new_mem_buf(data.data(), static_cast<int>(data.size()));
    EVP_PKEY *key = bio ? PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr) : nullptr;
    if (bio) BIO_free(bio);
    return key;
}

bool VerifySignature(const std::vector<unsigned char> &payload, const std::vector<unsigned char> &signature,
                     EVP_PKEY *key)
{
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    if (!context) return false;
    int valid = EVP_DigestVerifyInit(context, nullptr, nullptr, nullptr, key);
    if (valid == 1)
        valid = EVP_DigestVerify(context, signature.data(), signature.size(), payload.data(), payload.size());
    EVP_MD_CTX_free(context);
    return valid == 1;
}

std::string RequiredString(const nlohmann::json &value, const char *field)
{
    if (!value.contains(field) || !value.at(field).is_string() || value.at(field).get<std::string>().empty())
        throw std::runtime_error(std::string("manifest field '") + field + "' must be a non-empty string");
    return value.at(field).get<std::string>();
}
} // namespace

VerifiedPluginArtifact::VerifiedPluginArtifact(const VerifiedPluginArtifact &other)
    : Name(other.Name), Language(other.Language), Path(other.Path), Sha256(other.Sha256), Classes(other.Classes),
      Source(other.Source), Origin(other.Origin), m_Descriptor(other.m_Descriptor < 0 ? -1 : dup(other.m_Descriptor))
{
    if (other.m_Descriptor >= 0 && m_Descriptor < 0) throw std::runtime_error("cannot duplicate plugin descriptor");
}

VerifiedPluginArtifact &VerifiedPluginArtifact::operator=(const VerifiedPluginArtifact &other)
{
    if (this == &other) return *this;
    VerifiedPluginArtifact copy(other);
    *this = std::move(copy);
    return *this;
}

VerifiedPluginArtifact::VerifiedPluginArtifact(VerifiedPluginArtifact &&other) noexcept
    : Name(std::move(other.Name)), Language(std::move(other.Language)), Path(std::move(other.Path)),
      Sha256(std::move(other.Sha256)), Classes(std::move(other.Classes)), Source(std::move(other.Source)),
      Origin(std::move(other.Origin)), m_Descriptor(other.m_Descriptor)
{
    other.m_Descriptor = -1;
}

VerifiedPluginArtifact &VerifiedPluginArtifact::operator=(VerifiedPluginArtifact &&other) noexcept
{
    if (this == &other) return *this;
    if (m_Descriptor >= 0) close(m_Descriptor);
    Name = std::move(other.Name);
    Language = std::move(other.Language);
    Path = std::move(other.Path);
    Sha256 = std::move(other.Sha256);
    Classes = std::move(other.Classes);
    Source = std::move(other.Source);
    Origin = std::move(other.Origin);
    m_Descriptor = other.m_Descriptor;
    other.m_Descriptor = -1;
    return *this;
}

VerifiedPluginArtifact::~VerifiedPluginArtifact()
{
    if (m_Descriptor >= 0) close(m_Descriptor);
}

std::string PluginVerifier::IndexFingerprint(const std::vector<std::string> &pluginRoots,
                                             const std::vector<std::string> &trustStores)
{
    std::ostringstream snapshot;
    for (const std::string &root : pluginRoots) AppendTrackedFiles(snapshot, root, false);
    for (const std::string &root : trustStores) AppendTrackedFiles(snapshot, root, true);
    return Sha256(snapshot.str());
}

PluginDiscoveryResult PluginVerifier::Discover(const std::vector<std::string> &pluginRoots,
                                               PluginTrustPolicy policy, const std::string &language)
{
    PluginDiscoveryResult result;
    for (const std::string &rootValue : pluginRoots)
    {
        const fs::path root(rootValue);
        IndexReadLock indexLock(root);
        std::error_code error;
        if (!fs::is_directory(root, error) || error) continue;
        std::vector<fs::path> packages;
        for (fs::directory_iterator iterator(root, fs::directory_options::skip_permission_denied, error), end;
             !error && iterator != end; iterator.increment(error))
        {
            if (iterator->symlink_status(error).type() == fs::file_type::directory) packages.push_back(iterator->path());
            error.clear();
        }
        std::sort(packages.begin(), packages.end());
        const std::string trustStore = PluginPaths::TrustStoreForRoot(root.string());
        for (const auto &package : packages)
        {
            try
            {
                result.Packages.push_back(VerifyPackage(package.string(), trustStore, policy, language));
            }
            catch (const std::exception &failure)
            {
                result.Errors.push_back(package.string() + ": " + failure.what());
            }
        }
    }
    return result;
}

std::string PluginVerifier::HashFile(const std::string &path) { return Sha256(ReadRegularFile(path)); }

std::string PluginVerifier::HashBytes(const std::string &data) { return Sha256(data); }

std::string PluginVerifier::ReadFile(const std::string &path)
{
    const auto bytes = ReadRegularFile(path);
    return {bytes.begin(), bytes.end()};
}

void PluginVerifier::ValidateDirectoryTree(const std::string &path, bool allowMissingLeaf)
{
    fs::path current;
    const fs::path absolute = fs::absolute(path).lexically_normal();
    for (const auto &component : absolute)
    {
        current /= component;
        std::error_code error;
        const fs::file_status status = fs::symlink_status(current, error);
        if (error || status.type() == fs::file_type::not_found)
        {
            if (allowMissingLeaf) return;
            throw std::runtime_error("Plugin path does not exist: " + current.string());
        }
        if (fs::is_symlink(status)) throw std::runtime_error("Symbolic links are not allowed in plugin paths: " + current.string());
        if (!fs::is_directory(status)) throw std::runtime_error("Plugin path component is not a directory: " + current.string());
    }
}

void PluginVerifier::ValidateStagedTree(const std::string &packageDirectory)
{
    const fs::path root = fs::absolute(packageDirectory).lexically_normal();
    std::error_code error;
    const auto rootStatus = fs::symlink_status(root, error);
    if (error || !fs::is_directory(rootStatus) || fs::is_symlink(rootStatus))
        throw std::runtime_error("Staged plugin package must be a real directory: " + root.string());
    for (fs::recursive_directory_iterator iterator(root, fs::directory_options::skip_permission_denied, error), end;
         !error && iterator != end; iterator.increment(error))
    {
        const auto status = iterator->symlink_status(error);
        if (error) throw std::runtime_error("Cannot inspect staged plugin: " + iterator->path().string());
        if (fs::is_symlink(status)) throw std::runtime_error("Staged plugin contains a symbolic link: " + iterator->path().string());
        if (!fs::is_directory(status) && !fs::is_regular_file(status))
            throw std::runtime_error("Staged plugin contains a non-regular file: " + iterator->path().string());
    }
    if (error) throw std::runtime_error("Cannot inspect staged plugin tree: " + root.string());
}

VerifiedPluginPackage PluginVerifier::VerifyPackage(const std::string &packageDirectory, const std::string &trustStore,
                                                    PluginTrustPolicy policy, const std::string &language,
                                                    const std::string &trustedKey)
{
    const fs::path packageDir = fs::absolute(packageDirectory).lexically_normal();
    PackageReadLock packageLock(packageDir);
    if (!fs::is_directory(packageDir) || fs::is_symlink(fs::symlink_status(packageDir)))
        throw std::runtime_error("plugin package is not a real directory: " + packageDir.string());
    const std::string packageName = packageDir.filename().string();
    static const std::regex packagePattern("[A-Za-z0-9][A-Za-z0-9_.-]*");
    if (!std::regex_match(packageName, packagePattern)) throw std::runtime_error("invalid plugin package name");

    const fs::path manifestPath = packageDir / "plugin_manifest.json";
    const fs::path signaturePath = packageDir / "plugin_manifest.json.sig";
    const auto manifestBytes = ReadRegularFile(manifestPath);
    const bool hasSignature = fs::is_regular_file(signaturePath);

    VerifiedPluginPackage result;
    result.Package = packageName;
    result.ManifestPath = fs::canonical(manifestPath).string();
    result.ManifestSha256 = Sha256(manifestBytes);

    if (hasSignature)
    {
        const fs::path keyPath = trustedKey.empty() ? fs::path(trustStore) / (packageName + ".pem") : fs::path(trustedKey);
        if (!fs::is_regular_file(keyPath))
            throw std::runtime_error("signed plugin requires its package-bound trusted key: " + keyPath.string());
        const auto keyBytes = ReadRegularFile(keyPath);
        const auto signatureBytes = ReadRegularFile(signaturePath);
        EVP_PKEY *key = ReadPublicKey(keyBytes);
        if (!key) throw std::runtime_error("cannot parse trusted plugin key: " + keyPath.string());
        const bool valid = VerifySignature(manifestBytes, signatureBytes, key);
        EVP_PKEY_free(key);
        if (!valid) throw std::runtime_error("plugin manifest signature is invalid: " + manifestPath.string());
        result.Trust = PluginTrustStatus::Signed;
        result.TrustedKeyPath = fs::canonical(keyPath).string();
        result.SignerFingerprint = Sha256(keyBytes);
    }
    else if (policy == PluginTrustPolicy::RequireSigned)
    {
        throw std::runtime_error("plugin package requires a trusted signature: " + manifestPath.string());
    }

    const nlohmann::json manifest = nlohmann::json::parse(manifestBytes.begin(), manifestBytes.end());
    if (!manifest.is_object() || !manifest.contains("schema") || !manifest.at("schema").is_number_integer() ||
        manifest.at("schema").get<int>() != 2)
        throw std::runtime_error("unsupported plugin manifest schema");
    if (RequiredString(manifest, "package") != packageName)
        throw std::runtime_error("manifest package name does not match its directory");
    if (!manifest.contains("modules") || !manifest.at("modules").is_array())
        throw std::runtime_error("manifest field 'modules' must be an array");

    std::vector<std::string> paths;
    std::vector<std::string> identities;
    for (const auto &entry : manifest.at("modules"))
    {
        if (!entry.is_object()) throw std::runtime_error("manifest module entry must be an object");
        const std::string entryLanguage = RequiredString(entry, "language");
        if (entryLanguage != "cpp" && entryLanguage != "python")
            throw std::runtime_error("manifest module language must be 'cpp' or 'python'");
        const std::string name = RequiredString(entry, "name");
        const std::string relativeText = RequiredString(entry, "path");
        const std::string expectedHash = RequiredString(entry, "sha256");
        static const std::regex hashPattern("[0-9a-f]{64}");
        if (!std::regex_match(expectedHash, hashPattern)) throw std::runtime_error("invalid artifact sha256: " + name);
        const fs::path relative(relativeText);
        if (relative.is_absolute() || relative.has_root_path() || relativeText.find('\\') != std::string::npos)
            throw std::runtime_error("invalid plugin artifact path: " + relativeText);
        for (const auto &component : relative)
            if (component == "." || component == "..")
                throw std::runtime_error("plugin artifact path must be normalized: " + relativeText);
        if (std::find(paths.begin(), paths.end(), relativeText) != paths.end())
            throw std::runtime_error("duplicate plugin artifact path: " + relativeText);
        paths.push_back(relativeText);

        std::vector<std::string> classes;
        if (entryLanguage == "python")
        {
            if (!entry.contains("classes") || !entry.at("classes").is_array())
                throw std::runtime_error("python manifest entry requires a classes array: " + name);
            for (const auto &classValue : entry.at("classes"))
            {
                if (!classValue.is_string() || classValue.get<std::string>().empty())
                    throw std::runtime_error("python module classes must be non-empty strings: " + name);
                const std::string className = classValue.get<std::string>();
                if (std::find(identities.begin(), identities.end(), className) != identities.end())
                    throw std::runtime_error("duplicate plugin module identity: " + className);
                identities.push_back(className);
                classes.push_back(className);
            }
        }
        else
        {
            if (std::find(identities.begin(), identities.end(), name) != identities.end())
                throw std::runtime_error("duplicate plugin module identity: " + name);
            identities.push_back(name);
        }

        const fs::path artifactPath = packageDir / relative;
        if (!fs::is_regular_file(artifactPath) || !IsContainedPath(packageDir, artifactPath) ||
            HasSymlinkComponent(packageDir, artifactPath))
            throw std::runtime_error("plugin artifact is missing or escapes its package: " + artifactPath.string());
        if (entryLanguage == "cpp" &&
            (artifactPath.filename().string().size() < 9 ||
             artifactPath.filename().string().rfind("Module.so") != artifactPath.filename().string().size() - 9))
            throw std::runtime_error("C++ plugin filename must end with Module.so: " + artifactPath.string());

        int descriptor = OpenRegularFile(artifactPath);
        std::vector<unsigned char> artifactBytes;
        try
        {
            const std::string actualHash = Sha256Descriptor(descriptor);
            if (actualHash != expectedHash)
                throw std::runtime_error("plugin artifact hash mismatch: " + artifactPath.string());
            if (entryLanguage == "python") artifactBytes = ReadDescriptor(descriptor);
        }
        catch (...)
        {
            close(descriptor);
            throw;
        }
        const std::string actualHash = expectedHash;

        if (!language.empty() && entryLanguage != language)
        {
            close(descriptor);
            continue;
        }
        VerifiedPluginArtifact artifact;
        artifact.Name = name;
        artifact.Language = entryLanguage;
        artifact.Path = fs::canonical(artifactPath).string();
        artifact.Sha256 = actualHash;
        artifact.Classes = std::move(classes);
        if (entryLanguage == "python")
            artifact.Source.assign(reinterpret_cast<const char *>(artifactBytes.data()), artifactBytes.size());
        artifact.Origin.Package = packageName;
        artifact.Origin.ManifestPath = result.ManifestPath;
        artifact.Origin.ManifestSha256 = result.ManifestSha256;
        artifact.Origin.ArtifactSha256 = actualHash;
        artifact.Origin.SignerFingerprint = result.SignerFingerprint;
        artifact.Origin.Trust = result.Trust;
        artifact.m_Descriptor = descriptor;
        result.Artifacts.push_back(std::move(artifact));
    }
    std::sort(result.Artifacts.begin(), result.Artifacts.end(),
              [](const auto &left, const auto &right) { return left.Path < right.Path; });
    return result;
}
