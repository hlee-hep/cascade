#include "Bindings.hh"
#include "PluginPaths.hh"
#include "PluginVerifier.hh"
#include <pybind11/stl.h>

namespace py = pybind11;

namespace cascade::python_binding
{
void BindPlugins(py::module_ &m)
{
    py::enum_<PluginTrustPolicy>(m, "PluginTrustPolicy")
        .value("Verified", PluginTrustPolicy::Verified)
        .value("RequireSigned", PluginTrustPolicy::RequireSigned);
    py::enum_<PluginTrustStatus>(m, "PluginTrustStatus")
        .value("Verified", PluginTrustStatus::Verified)
        .value("Signed", PluginTrustStatus::Signed);
    py::class_<PluginOrigin>(m, "PluginOrigin")
        .def_readonly("package", &PluginOrigin::Package)
        .def_readonly("manifest_path", &PluginOrigin::ManifestPath)
        .def_readonly("manifest_sha256", &PluginOrigin::ManifestSha256)
        .def_readonly("artifact_sha256", &PluginOrigin::ArtifactSha256)
        .def_readonly("signer_fingerprint", &PluginOrigin::SignerFingerprint)
        .def_readonly("trust", &PluginOrigin::Trust);
    py::class_<VerifiedPluginArtifact>(m, "VerifiedPluginArtifact")
        .def_readonly("name", &VerifiedPluginArtifact::Name)
        .def_readonly("language", &VerifiedPluginArtifact::Language)
        .def_readonly("path", &VerifiedPluginArtifact::Path)
        .def_readonly("sha256", &VerifiedPluginArtifact::Sha256)
        .def_readonly("classes", &VerifiedPluginArtifact::Classes)
        .def_property_readonly("source", [](const VerifiedPluginArtifact &artifact) { return py::bytes(artifact.Source); })
        .def_readonly("origin", &VerifiedPluginArtifact::Origin);
    py::class_<VerifiedPluginPackage>(m, "VerifiedPluginPackage")
        .def_readonly("package", &VerifiedPluginPackage::Package)
        .def_readonly("manifest_path", &VerifiedPluginPackage::ManifestPath)
        .def_readonly("manifest_sha256", &VerifiedPluginPackage::ManifestSha256)
        .def_readonly("trusted_key_path", &VerifiedPluginPackage::TrustedKeyPath)
        .def_readonly("signer_fingerprint", &VerifiedPluginPackage::SignerFingerprint)
        .def_readonly("trust", &VerifiedPluginPackage::Trust)
        .def_readonly("artifacts", &VerifiedPluginPackage::Artifacts);
    py::class_<PluginDiscoveryResult>(m, "PluginDiscoveryResult")
        .def_readonly("packages", &PluginDiscoveryResult::Packages)
        .def_readonly("errors", &PluginDiscoveryResult::Errors);
    py::class_<PluginManifestEntry>(m, "PluginManifestEntry")
        .def_readonly("package", &PluginManifestEntry::Package)
        .def_readonly("manifest_path", &PluginManifestEntry::ManifestPath)
        .def_readonly("language", &PluginManifestEntry::Language)
        .def_readonly("name", &PluginManifestEntry::Name)
        .def_readonly("identity", &PluginManifestEntry::Identity)
        .def_readonly("artifact_path", &PluginManifestEntry::ArtifactPath)
        .def_readonly("declared_sha256", &PluginManifestEntry::DeclaredSha256)
        .def_readonly("metadata", &PluginManifestEntry::Metadata)
        .def_readonly("has_signature", &PluginManifestEntry::HasSignature);
    py::class_<PluginManifestIndexResult>(m, "PluginManifestIndexResult")
        .def_readonly("entries", &PluginManifestIndexResult::Entries)
        .def_readonly("errors", &PluginManifestIndexResult::Errors);
    py::class_<PluginVerifier>(m, "PluginVerifier")
        .def_static("index_fingerprint",
                    [](const std::vector<std::string> &pluginRoots, const std::vector<std::string> &trustStores)
                    {
                        py::gil_scoped_release release;
                        return PluginVerifier::IndexFingerprint(pluginRoots, trustStores);
                    },
                    py::arg("plugin_roots"), py::arg("trust_stores"))
        .def_static("index_manifests",
                    [](const std::vector<std::string> &pluginRoots, const std::string &language)
                    {
                        py::gil_scoped_release release;
                        return PluginVerifier::IndexManifests(pluginRoots, language);
                    },
                    py::arg("plugin_roots"), py::arg("language") = "")
        .def_static("discover",
                    [](const std::vector<std::string> &pluginRoots, PluginTrustPolicy policy,
                       const std::string &language)
                    {
                        py::gil_scoped_release release;
                        return PluginVerifier::Discover(pluginRoots, policy, language);
                    },
                    py::arg("plugin_roots"), py::arg("policy") = PluginTrustPolicy::Verified,
                    py::arg("language") = "")
        .def_static("hash_file", [](const std::string &path)
                    { py::gil_scoped_release release; return PluginVerifier::HashFile(path); })
        .def_static("hash_bytes", [](const py::bytes &data)
                    { return PluginVerifier::HashBytes(py::cast<std::string>(data)); })
        .def_static("read_file", [](const std::string &path)
                    {
                        std::string data;
                        {
                            py::gil_scoped_release release;
                            data = PluginVerifier::ReadFile(path);
                        }
                        return py::bytes(data);
                    })
        .def_static("validate_directory_tree", [](const std::string &path, bool allowMissingLeaf)
                    { py::gil_scoped_release release; PluginVerifier::ValidateDirectoryTree(path, allowMissingLeaf); },
                    py::arg("path"), py::arg("allow_missing_leaf") = false)
        .def_static("validate_staged_tree", [](const std::string &path)
                    { py::gil_scoped_release release; PluginVerifier::ValidateStagedTree(path); })
        .def_static("verify_package",
                    [](const std::string &packageDirectory, const std::string &trustStore, PluginTrustPolicy policy,
                       const std::string &language, const std::string &trustedKey,
                       const std::string &moduleIdentity)
                    {
                        py::gil_scoped_release release;
                        return PluginVerifier::VerifyPackage(packageDirectory, trustStore, policy, language, trustedKey,
                                                             moduleIdentity);
                    },
                    py::arg("package_directory"), py::arg("trust_store"),
                    py::arg("policy") = PluginTrustPolicy::Verified, py::arg("language") = "",
                    py::arg("trusted_key") = "", py::arg("module_identity") = "");
    py::class_<PluginLayout>(m, "PluginLayout")
        .def_readonly("prefix", &PluginLayout::Prefix)
        .def_readonly("cpp", &PluginLayout::Cpp)
        .def_readonly("python", &PluginLayout::Python)
        .def_readonly("include", &PluginLayout::Include)
        .def_readonly("trust_store", &PluginLayout::TrustStore)
        .def_readonly("source", &PluginLayout::Source);
    py::class_<PluginPaths>(m, "PluginPaths")
        .def_static("config_path", &PluginPaths::ConfigPath)
        .def_static("runtime_prefix", &PluginPaths::RuntimePrefix)
        .def_static("canonical_prefix", &PluginPaths::CanonicalPrefix)
        .def_static("unique", &PluginPaths::Unique)
        .def_static("load_config",
                    [](const std::string &path)
                    {
                        const auto config = PluginPaths::LoadConfig(path);
                        py::list entries;
                        for (const auto &entry : config.Prefixes)
                        {
                            py::dict item;
                            item["path"] = entry.Path;
                            item["enabled"] = entry.Enabled;
                            entries.append(std::move(item));
                        }
                        py::dict result;
                        result["schema"] = config.Schema;
                        result["plugin_prefixes"] = std::move(entries);
                        return result;
                    },
                    py::arg("path") = "")
        .def_static("configured_prefixes", &PluginPaths::ConfiguredPrefixes, py::arg("path") = "")
        .def_static("add_prefix", &PluginPaths::AddPrefix, py::arg("prefix"), py::arg("path") = "")
        .def_static("remove_prefix", &PluginPaths::RemovePrefix, py::arg("prefix"), py::arg("path") = "")
        .def_static("layout", &PluginPaths::Layout)
        .def_static("runtime_layouts", &PluginPaths::RuntimeLayouts)
        .def_static("roots", &PluginPaths::Roots)
        .def_static("trust_store_for_root", &PluginPaths::TrustStoreForRoot);
}
} // namespace cascade::python_binding
