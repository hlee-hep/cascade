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


class DummyBaseModule:
    pass


def _load_controller():
    cascade = types.ModuleType("cascade")
    cascade.__file__ = str(pathlib.Path(__file__).parents[1] / "python" / "__init__.py")
    cascade.init_interrupt = lambda: None
    cascade.is_interrupted = lambda: False
    cascade.log = lambda *args, **kwargs: None
    cascade.log_level = types.SimpleNamespace(INFO=1, WARN=2, ERROR=3)
    sys.modules["cascade"] = cascade

    pymodule = types.ModuleType("cascade.pymodule")
    pymodule.base_module = DummyBaseModule
    sys.modules["cascade.pymodule"] = pymodule

    extension = types.ModuleType("cascade._cascade")
    extension.AMCM = type("AMCM", (), {"__init__": lambda self, *args, **kwargs: None})
    extension.IAnalysisModule = type("IAnalysisModule", (), {})
    extension.PluginTrustPolicy = types.SimpleNamespace(Verified="verified", RequireSigned="require-signed")
    sys.modules["cascade._cascade"] = extension

    path = pathlib.Path(__file__).parents[1] / "python" / "py_amcm.py"
    spec = importlib.util.spec_from_file_location("cascade_test_py_amcm", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _sha256(path):
    digest = hashlib.sha256()
    digest.update(path.read_bytes())
    return digest.hexdigest()


class PluginPackageTests(unittest.TestCase):
    def test_multiple_signed_packages_coexist(self):
        controller = _load_controller()
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
                self.assertTrue(all(info["origin"]["trust"] == "Signed" for info in index.values()))
                first = controller._import_python_plugin(index["FirstModule"])
                second = controller._import_python_plugin(index["SecondModule"])
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
        controller = _load_controller()
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
                self.assertEqual(verified["LocalModule"]["origin"]["trust"], "Verified")

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


if __name__ == "__main__":
    unittest.main()
