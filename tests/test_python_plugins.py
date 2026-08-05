import hashlib
import importlib.util
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

from tests.module_isolation import restore_package_modules, snapshot_package_modules


class DummyBaseModule:
    pass


def _load_controller(test_case):
    previous_modules = snapshot_package_modules("cascade")
    cascade = types.ModuleType("cascade")
    cascade.__file__ = str(pathlib.Path(__file__).parents[1] / "python" / "__init__.py")
    cascade.init_interrupt = lambda: None
    cascade.is_interrupted = lambda: False
    cascade.log = lambda *args, **kwargs: None
    cascade.log_level = types.SimpleNamespace(DEBUG=0, INFO=1, WARN=2, WARNING=2, ERROR=3)
    sys.modules["cascade"] = cascade

    pymodule = types.ModuleType("cascade.pymodule")
    pymodule.base_module = DummyBaseModule
    sys.modules["cascade.pymodule"] = pymodule

    extension = types.ModuleType("cascade._cascade")
    extension.AMCM = type("AMCM", (), {"__init__": lambda self, *args, **kwargs: None})
    extension.IAnalysisModule = type("IAnalysisModule", (), {})
    extension.PluginTrustPolicy = types.SimpleNamespace(Verified="verified", RequireSigned="require-signed")

    class TestPluginVerifier:
        @staticmethod
        def index_fingerprint(plugin_roots, trust_stores):
            tracked = []
            for root in [*plugin_roots, *trust_stores]:
                root_path = pathlib.Path(root)
                if not root_path.is_dir():
                    tracked.append((str(root_path), None, None))
                    continue
                for path in root_path.rglob("*"):
                    if path.is_file() and (path.name in {"plugin_manifest.json", "plugin_manifest.json.sig"} or path.suffix == ".pem"):
                        metadata = path.stat()
                        tracked.append((str(path.resolve()), metadata.st_mtime_ns, metadata.st_size))
            return hashlib.sha256(repr(sorted(tracked)).encode("utf-8")).hexdigest()

        @staticmethod
        def index_manifests(plugin_roots, language=""):
            entries = []
            errors = []
            for plugin_root in plugin_roots:
                root_path = pathlib.Path(plugin_root)
                if not root_path.is_dir():
                    continue
                for package in sorted(root_path.iterdir()):
                    manifest_path = package / "plugin_manifest.json"
                    if not package.is_dir() or package.is_symlink() or not manifest_path.is_file():
                        continue
                    try:
                        document = json.loads(manifest_path.read_text(encoding="utf-8"))
                        if document.get("schema") != 2 or document.get("package") != package.name:
                            raise RuntimeError("invalid plugin manifest")
                        for item in document.get("modules", []):
                            item_language = item["language"]
                            if language and item_language != language:
                                continue
                            identities = item.get("classes", []) if item_language == "python" else [item["name"]]
                            for identity in identities:
                                metadata = item.get("class_metadata", {}).get(identity, item.get("metadata", {}))
                                entries.append(types.SimpleNamespace(
                                    package=package.name,
                                    manifest_path=str(manifest_path.resolve()),
                                    language=item_language,
                                    name=item["name"],
                                    identity=identity,
                                    artifact_path=str((package / item["path"]).resolve()),
                                    declared_sha256=item["sha256"],
                                    metadata=types.SimpleNamespace(
                                        name=metadata.get("name", identity),
                                        version=metadata.get("version", ""),
                                        summary=metadata.get("summary", ""),
                                        tags=metadata.get("tags", []),
                                    ),
                                    has_signature=pathlib.Path(str(manifest_path) + ".sig").is_file(),
                                ))
                    except Exception as error:
                        errors.append(f"{manifest_path}: {error}")
            return types.SimpleNamespace(entries=entries, errors=errors)

        @staticmethod
        def verify_package(package_dir, trust_store, policy, language, trusted_key="", module_identity=""):
            package_path = pathlib.Path(package_dir)
            manifest_path = package_path / "plugin_manifest.json"
            manifest_bytes = manifest_path.read_bytes()
            document = json.loads(manifest_bytes)
            signature_path = pathlib.Path(str(manifest_path) + ".sig")
            key_path = pathlib.Path(trusted_key) if trusted_key else pathlib.Path(trust_store) / f"{package_path.name}.pem"
            signed = signature_path.is_file()
            if signed:
                if not key_path.is_file():
                    raise RuntimeError("signed plugin requires its package-bound trusted key")
                result = subprocess.run(
                    [
                        "openssl", "pkeyutl", "-verify", "-pubin", "-inkey", str(key_path),
                        "-rawin", "-in", str(manifest_path), "-sigfile", str(signature_path),
                    ],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
                if result.returncode:
                    raise RuntimeError("plugin manifest signature is invalid")
            elif policy == "require-signed":
                raise RuntimeError("plugin package requires a trusted signature")
            artifacts = []
            for entry in document["modules"]:
                if entry["language"] != language:
                    continue
                identities = entry.get("classes", []) if language == "python" else [entry["name"]]
                if module_identity and module_identity not in identities:
                    continue
                artifact_path = package_path / entry["path"]
                source = artifact_path.read_bytes()
                if hashlib.sha256(source).hexdigest() != entry["sha256"]:
                    raise RuntimeError("plugin artifact hash mismatch")
                artifacts.append(types.SimpleNamespace(
                    name=entry["name"],
                    path=str(artifact_path.resolve()),
                    sha256=entry["sha256"],
                    classes=entry.get("classes", []),
                    source=source,
                ))
            return types.SimpleNamespace(
                package=package_path.name,
                manifest_path=str(manifest_path.resolve()),
                manifest_sha256=hashlib.sha256(manifest_bytes).hexdigest(),
                trusted_key_path=str(key_path.resolve()) if signed else "",
                signer_fingerprint=hashlib.sha256(key_path.read_bytes()).hexdigest() if signed else "",
                trust="Signed" if signed else "Verified",
                artifacts=artifacts,
            )

        @staticmethod
        def discover(plugin_roots, policy, language):
            packages = []
            errors = []
            for plugin_root in plugin_roots:
                root_path = pathlib.Path(plugin_root)
                if not root_path.is_dir():
                    continue
                trust_store = TestPluginPaths.trust_store_for_root(str(root_path))
                for package in sorted(root_path.iterdir()):
                    if not package.is_dir() or package.is_symlink():
                        continue
                    try:
                        packages.append(TestPluginVerifier.verify_package(
                            str(package), trust_store, policy, language
                        ))
                    except Exception as error:
                        errors.append(f"{package}: {error}")
            return types.SimpleNamespace(packages=packages, errors=errors)

    class TestPluginPaths:
        @staticmethod
        def roots(language):
            roots = []
            configured = os.environ.get("CASCADE_PYPLUGIN_DIR")
            if configured and language == "python":
                roots.append(configured)
            config_path = os.environ.get("CASCADE_CONFIG_FILE")
            if config_path and pathlib.Path(config_path).is_file():
                document = json.loads(pathlib.Path(config_path).read_text(encoding="utf-8"))
                for entry in document.get("plugin_prefixes", []):
                    if isinstance(entry, str):
                        prefix, enabled = entry, True
                    else:
                        prefix, enabled = entry["path"], entry.get("enabled", True)
                    if enabled:
                        leaf = "pyplugin" if language == "python" else "plugin"
                        roots.append(str(pathlib.Path(prefix) / "lib" / "cascade" / leaf))
            leaf = "pyplugin" if language == "python" else "plugin"
            roots.append(str(pathlib.Path(cascade.__file__).parent / leaf))
            return list(dict.fromkeys(str(pathlib.Path(root).resolve()) for root in roots))

        @staticmethod
        def trust_store_for_root(plugin_root):
            configured = os.environ.get("CASCADE_PLUGIN_TRUST_STORE")
            if configured:
                return str(pathlib.Path(configured).resolve())
            prefix = pathlib.Path(plugin_root).resolve()
            for _ in range(3):
                prefix = prefix.parent
            return str(prefix / "share" / "cascade" / "trusted_keys")

        @staticmethod
        def unique(paths):
            return list(dict.fromkeys(str(pathlib.Path(path).resolve()) for path in paths))

    extension.PluginVerifier = TestPluginVerifier
    extension.PluginPaths = TestPluginPaths
    sys.modules["cascade._cascade"] = extension

    try:
        path = pathlib.Path(__file__).parents[1] / "python" / "py_amcm.py"
        spec = importlib.util.spec_from_file_location("cascade_test_py_amcm", path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
    except Exception:
        restore_package_modules("cascade", previous_modules)
        raise
    test_case.addCleanup(restore_package_modules, "cascade", previous_modules)
    return module


def _sha256(path):
    digest = hashlib.sha256()
    digest.update(path.read_bytes())
    return digest.hexdigest()


class PluginPackageTests(unittest.TestCase):
    def test_persistent_prefix_is_discovered_without_plugin_root_environment(self):
        controller = _load_controller(self)
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            prefix = root / "plugin-prefix"
            plugin_root = prefix / "lib" / "cascade" / "pyplugin"
            package = plugin_root / "persistent-package"
            package.mkdir(parents=True)
            source = package / "persistent_module.py"
            source.write_text(
                "from cascade.pymodule import base_module\n"
                "class PersistentModule(base_module):\n"
                "    pass\n",
                encoding="utf-8",
            )
            (package / "plugin_manifest.json").write_text(
                json.dumps({
                    "schema": 2,
                    "package": package.name,
                    "modules": [{
                        "name": "persistent_module",
                        "language": "python",
                        "path": source.name,
                        "sha256": _sha256(source),
                        "classes": ["PersistentModule"],
                    }],
                }),
                encoding="utf-8",
            )
            config = root / "config.json"
            config.write_text(
                json.dumps({
                    "schema": 1,
                    "plugin_prefixes": [{"path": str(prefix), "enabled": True}],
                }),
                encoding="utf-8",
            )
            environment = {"CASCADE_CONFIG_FILE": str(config)}
            with mock.patch.dict(os.environ, environment, clear=False):
                os.environ.pop("CASCADE_PYPLUGIN_DIR", None)
                os.environ.pop("CASCADE_PLUGIN_TRUST_STORE", None)
                controller._PYPLUGIN_CACHE = None
                controller._PYPLUGIN_CACHE_KEY = None
                index = controller._load_python_plugin_index()
            self.assertEqual(set(index), {"PersistentModule"})
            self.assertEqual(index["PersistentModule"]["manifest"], str((package / "plugin_manifest.json").resolve()))

    def test_multiple_signed_packages_coexist(self):
        controller = _load_controller(self)
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            plugin_root = root / "pyplugin"
            trust_store = root / "trusted_keys"
            plugin_root.mkdir()
            trust_store.mkdir()
            private_key = root / "private.pem"
            public_key = trust_store / "publisher.pem"
            subprocess.check_call(["openssl", "genpkey", "-algorithm", "ED25519", "-out", str(private_key)])
            subprocess.check_call(["openssl", "pkey", "-in", str(private_key), "-pubout", "-out", str(public_key)])

            def install_package(package_name, class_name):
                package = plugin_root / package_name
                package.mkdir()
                (trust_store / f"{package_name}.pem").write_bytes(public_key.read_bytes())
                source = package / "shared_module.py"
                source.write_text(
                    "from cascade.pymodule import base_module\n"
                    f"class {class_name}(base_module):\n"
                    "    pass\n",
                    encoding="utf-8",
                )
                manifest = package / "plugin_manifest.json"
                manifest.write_text(
                    json.dumps(
                        {
                            "schema": 2,
                            "package": package_name,
                            "modules": [
                                {
                                    "name": package_name,
                                    "language": "python",
                                    "path": source.name,
                                    "sha256": _sha256(source),
                                    "classes": [class_name],
                                }
                            ],
                        },
                        indent=2,
                    )
                    + "\n",
                    encoding="utf-8",
                )
                subprocess.check_call(
                    [
                        "openssl",
                        "pkeyutl",
                        "-sign",
                        "-inkey",
                        str(private_key),
                        "-rawin",
                        "-in",
                        str(manifest),
                        "-out",
                        str(manifest) + ".sig",
                    ]
                )

            install_package("package-one", "FirstModule")
            install_package("package_one", "SecondModule")

            previous_root = os.environ.get("CASCADE_PYPLUGIN_DIR")
            previous_trust = os.environ.get("CASCADE_PLUGIN_TRUST_STORE")
            try:
                os.environ["CASCADE_PYPLUGIN_DIR"] = str(plugin_root)
                os.environ["CASCADE_PLUGIN_TRUST_STORE"] = str(trust_store)
                controller._PYPLUGIN_CACHE = None
                controller._PYPLUGIN_CACHE_KEY = None
                index = controller._load_python_plugin_index()
                self.assertEqual(set(index), {"FirstModule", "SecondModule"})
                first_info = controller._load_targeted_python_plugin_info(
                    index["FirstModule"]["manifest"], "FirstModule"
                )
                second_info = controller._load_targeted_python_plugin_info(
                    index["SecondModule"]["manifest"], "SecondModule"
                )
                self.assertEqual(first_info["origin"]["trust"], "Signed")
                self.assertEqual(second_info["origin"]["trust"], "Signed")
                first = controller._import_python_plugin(first_info)
                second = controller._import_python_plugin(second_info)
                self.assertTrue(hasattr(first, "FirstModule"))
                self.assertTrue(hasattr(second, "SecondModule"))
                self.assertNotEqual(first.__name__, second.__name__)

                install_package("package-three", "ThirdModule")
                refreshed = controller._load_python_plugin_index()
                self.assertEqual(
                    set(refreshed),
                    {"FirstModule", "SecondModule", "ThirdModule"},
                )
            finally:
                if previous_root is None:
                    os.environ.pop("CASCADE_PYPLUGIN_DIR", None)
                else:
                    os.environ["CASCADE_PYPLUGIN_DIR"] = previous_root
                if previous_trust is None:
                    os.environ.pop("CASCADE_PLUGIN_TRUST_STORE", None)
                else:
                    os.environ["CASCADE_PLUGIN_TRUST_STORE"] = previous_trust

    def test_unsigned_package_is_verified_but_rejected_by_signed_policy(self):
        controller = _load_controller(self)
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            plugin_root = root / "pyplugin"
            package = plugin_root / "local-package"
            package.mkdir(parents=True)
            source = package / "local_module.py"
            source.write_text(
                "from cascade.pymodule import base_module\n"
                "class LocalModule(base_module):\n"
                "    pass\n",
                encoding="utf-8",
            )
            manifest = package / "plugin_manifest.json"
            manifest.write_text(
                json.dumps({
                    "schema": 2,
                    "package": "local-package",
                    "modules": [{
                        "name": "local_module",
                        "language": "python",
                        "path": source.name,
                        "sha256": _sha256(source),
                        "classes": ["LocalModule"],
                    }],
                }),
                encoding="utf-8",
            )

            previous_root = os.environ.get("CASCADE_PYPLUGIN_DIR")
            previous_trust = os.environ.get("CASCADE_PLUGIN_TRUST_STORE")
            try:
                os.environ["CASCADE_PYPLUGIN_DIR"] = str(plugin_root)
                os.environ["CASCADE_PLUGIN_TRUST_STORE"] = str(root / "missing-trust-store")
                controller._PYPLUGIN_CACHE = None
                controller._PYPLUGIN_CACHE_KEY = None
                verified = controller._load_python_plugin_index()
                self.assertEqual(set(verified), {"LocalModule"})
                info = controller._load_targeted_python_plugin_info(
                    verified["LocalModule"]["manifest"], "LocalModule"
                )
                self.assertEqual(info["origin"]["trust"], "Verified")

                strict = controller._load_python_plugin_index(require_signed=True)
                self.assertEqual(strict, {})
            finally:
                if previous_root is None:
                    os.environ.pop("CASCADE_PYPLUGIN_DIR", None)
                else:
                    os.environ["CASCADE_PYPLUGIN_DIR"] = previous_root
                if previous_trust is None:
                    os.environ.pop("CASCADE_PLUGIN_TRUST_STORE", None)
                else:
                    os.environ["CASCADE_PLUGIN_TRUST_STORE"] = previous_trust

    def test_import_executes_the_exact_source_bytes_that_were_verified(self):
        controller = _load_controller(self)
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            plugin_root = root / "pyplugin"
            package = plugin_root / "immutable-package"
            package.mkdir(parents=True)
            source = package / "immutable_module.py"
            verified_source = (
                "from cascade.pymodule import base_module\n"
                "VALUE = 'verified'\n"
                "class ImmutableModule(base_module):\n"
                "    pass\n"
            )
            source.write_text(verified_source, encoding="utf-8")
            (package / "plugin_manifest.json").write_text(json.dumps({
                "schema": 2,
                "package": package.name,
                "modules": [{
                    "name": "immutable_module",
                    "language": "python",
                    "path": source.name,
                    "sha256": _sha256(source),
                    "classes": ["ImmutableModule"],
                }],
            }), encoding="utf-8")

            with mock.patch.dict(os.environ, {
                "CASCADE_PYPLUGIN_DIR": str(plugin_root),
                "CASCADE_PLUGIN_TRUST_STORE": str(root / "missing-trust-store"),
            }, clear=False):
                controller._PYPLUGIN_CACHE = None
                controller._PYPLUGIN_CACHE_KEY = None
                index = controller._load_python_plugin_index()
                info = controller._load_targeted_python_plugin_info(
                    index["ImmutableModule"]["manifest"], "ImmutableModule"
                )
                source.write_text("raise RuntimeError('replacement executed')\n", encoding="utf-8")
                loaded = controller._import_python_plugin(info)
                with self.assertRaisesRegex(RuntimeError, "hash mismatch"):
                    controller._load_targeted_python_plugin_info(
                        index["ImmutableModule"]["manifest"], "ImmutableModule"
                    )
            self.assertEqual(loaded.VALUE, "verified")


if __name__ == "__main__":
    unittest.main()
