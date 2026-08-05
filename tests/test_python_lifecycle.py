import importlib.util
import importlib.machinery
import ctypes
import json
import os
import pathlib
import signal
import subprocess
import sys
import tempfile
import types
import unittest


def _load_base_module():
    cascade = types.ModuleType("cascade")
    cascade.__version__ = "test"
    cascade.is_interrupted = lambda: False
    cascade.log = lambda *args, **kwargs: None
    cascade.log_level = types.SimpleNamespace(INFO=1, WARN=2, ERROR=3)
    sys.modules["cascade"] = cascade

    extension_path = pathlib.Path(__file__).parents[1] / "build" / "main" / "libCascade.so"
    build_root = pathlib.Path(__file__).parents[1] / "build"
    for library in (
        build_root / "utils" / "libutils.so",
        build_root / "ParamManager" / "libParamManager.so",
        build_root / "AnalysisManager" / "libAnalysisManager.so",
        build_root / "PlotManager" / "libPlotManager.so",
        build_root / "src" / "libAMCM.so",
    ):
        ctypes.CDLL(str(library), mode=ctypes.RTLD_GLOBAL)
    loader = importlib.machinery.ExtensionFileLoader("cascade._cascade", str(extension_path))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    extension = importlib.util.module_from_spec(spec)
    loader.exec_module(extension)
    cascade.CancellationToken = extension.CancellationToken
    cascade.CacheManager = extension.CacheManager
    cascade.OutputTransaction = extension.OutputTransaction
    cascade.ExecutionContext = extension.ExecutionContext
    cascade.IAnalysisModule = extension.IAnalysisModule
    cascade.ModulePhase = extension.ModulePhase
    cascade.ModuleStatus = extension.ModuleStatus
    cascade.ProvenanceRecorder = extension.ProvenanceRecorder
    cascade.PluginTrustPolicy = extension.PluginTrustPolicy
    cascade.PluginVerifier = extension.PluginVerifier
    cascade.ParamManager = extension.ParamManager
    cascade.SnapshotHasher = extension.SnapshotHasher
    cascade.RunResult = extension.RunResult

    path = pathlib.Path(__file__).parents[1] / "modules" / "python" / "base_module.py"
    spec = importlib.util.spec_from_file_location("cascade_test_base_module", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


base = _load_base_module()


class Module(base.base_module):
    def __init__(self, failure=None):
        super().__init__()
        self.basename = "Module"
        self.code_version_hash = "test"
        self.params["force_run"] = True
        self.failure = failure
        self.failure_hook = None

    def init(self):
        if self.failure is base.ModulePhase.INIT:
            raise RuntimeError("init failed")

    def execute(self):
        if self.failure is base.ModulePhase.EXECUTE:
            raise RuntimeError("execute failed")

    def finalize(self):
        if self.failure is base.ModulePhase.FINALIZE:
            raise RuntimeError("finalize failed")

    def snapshot_state(self):
        if self.failure is base.ModulePhase.CHECK:
            raise RuntimeError("check failed")
        return {}

    def on_failure(self, phase, message):
        self.failure_hook = (phase, message)


class OutputModule(Module):
    def execute(self):
        path = self.stage_output("result.txt")
        path.write_text("new", encoding="utf-8")
        super().execute()


class CrashAfterPromotionModule(Module):
    def execute(self):
        path = self.stage_output("result.txt")
        path.write_text("new", encoding="utf-8")
        self.context.outputs.commit()
        os.kill(os.getpid(), signal.SIGKILL)


class LifecycleTests(unittest.TestCase):
    def setUp(self):
        self.tempdir = tempfile.TemporaryDirectory()
        self.old_home = pathlib.Path.home
        self.old_output = os.environ.get("CASCADE_OUTPUT_DIR")
        self.old_cache = os.environ.get("CASCADE_CACHE_DIR")
        os.environ["CASCADE_OUTPUT_DIR"] = str(
            pathlib.Path(self.tempdir.name) / "default-output"
        )
        os.environ["CASCADE_CACHE_DIR"] = str(
            pathlib.Path(self.tempdir.name) / "default-cache"
        )

    def tearDown(self):
        if self.old_output is None:
            os.environ.pop("CASCADE_OUTPUT_DIR", None)
        else:
            os.environ["CASCADE_OUTPUT_DIR"] = self.old_output
        if self.old_cache is None:
            os.environ.pop("CASCADE_CACHE_DIR", None)
        else:
            os.environ["CASCADE_CACHE_DIR"] = self.old_cache
        self.tempdir.cleanup()

    def test_success(self):
        module = Module()
        result = module.run()
        self.assertEqual(result.status, base.ModuleStatus.DONE)
        self.assertTrue(result.succeeded())
        manifest_path = pathlib.Path(module.get_last_provenance_path())
        self.assertTrue(manifest_path.is_file())
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        self.assertEqual(manifest["schema"], "cascade.module-run")
        self.assertEqual(manifest["runtime"]["language"], "python")
        self.assertEqual(manifest["result"]["status"], "Done")
        self.assertEqual(len(manifest["identity"]["snapshot_hash"]), 64)
        self.assertTrue(sys.modules["cascade"].CacheManager.is_hash_cached(
            "Module", manifest["identity"]["snapshot_hash"], str(module.context.cache_directory)
        ))

    def test_plugin_origin_is_recorded_in_provenance(self):
        module = Module()
        module.set_plugin_origin({
            "package": "python-local",
            "trust": "Verified",
            "manifest_path": "/tmp/python-local/plugin_manifest.json",
            "manifest_sha256": "a" * 64,
            "artifact_sha256": "b" * 64,
            "signer_fingerprint": None,
        })
        self.assertTrue(module.run().succeeded())
        manifest = json.loads(
            pathlib.Path(module.get_last_provenance_path()).read_text(encoding="utf-8")
        )
        self.assertEqual(manifest["plugin"]["package"], "python-local")
        self.assertEqual(manifest["plugin"]["trust"], "Verified")

    def test_core_plugin_verifier_enforces_package_bound_signatures(self):
        import hashlib

        root = pathlib.Path(self.tempdir.name)
        package = root / "pyplugin" / "signed-package"
        trust_store = root / "trusted_keys"
        package.mkdir(parents=True)
        trust_store.mkdir()
        source = package / "signed_module.py"
        source.write_text("hello", encoding="utf-8")
        manifest = package / "plugin_manifest.json"
        manifest.write_text(json.dumps({
            "schema": 2,
            "package": package.name,
            "modules": [{
                "name": "signed_module",
                "language": "python",
                "path": source.name,
                "sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                "classes": ["SignedModule"],
            }],
        }), encoding="utf-8")
        private_key = root / "private.pem"
        public_key = trust_store / f"{package.name}.pem"
        subprocess.check_call([
            "openssl", "genpkey", "-algorithm", "ED25519", "-out", str(private_key)
        ])
        subprocess.check_call([
            "openssl", "pkey", "-in", str(private_key), "-pubout", "-out", str(public_key)
        ])
        subprocess.check_call([
            "openssl", "pkeyutl", "-sign", "-inkey", str(private_key), "-rawin",
            "-in", str(manifest), "-out", str(manifest) + ".sig",
        ])
        cascade = sys.modules["cascade"]
        verified = cascade.PluginVerifier.verify_package(
            str(package), str(trust_store), cascade.PluginTrustPolicy.Verified, "python"
        )
        self.assertEqual(verified.trust.name, "Signed")

        public_key.rename(trust_store / "another-package.pem")
        with self.assertRaisesRegex(RuntimeError, "package-bound trusted key"):
            cascade.PluginVerifier.verify_package(
                str(package), str(trust_store), cascade.PluginTrustPolicy.Verified, "python"
            )

    def test_each_failure_phase(self):
        for phase in (
            base.ModulePhase.INIT,
            base.ModulePhase.CHECK,
            base.ModulePhase.EXECUTE,
            base.ModulePhase.FINALIZE,
            base.ModulePhase.COMMIT,
        ):
            with self.subTest(phase=phase):
                module = Module(phase)
                if phase is base.ModulePhase.COMMIT:
                    cache_file = pathlib.Path(self.tempdir.name) / "cache-file"
                    cache_file.write_text("not a directory", encoding="utf-8")
                    module.set_cache_directory(cache_file)
                result = module.run()
                self.assertEqual(result.status, base.ModuleStatus.FAILED)
                self.assertEqual(result.phase, phase)
                self.assertIsNotNone(result.exception)
                self.assertEqual(module.failure_hook[0], phase)

    def test_parameter_types_are_stable(self):
        module = Module()
        self.assertEqual(base.ModuleStatus.DONE.value, "Done")
        self.assertEqual(base.ModulePhase.EXECUTE.value, "Execute")
        self.assertEqual(set(module.params), {"dry_run", "force_run"})
        module.register_param("count", 1)
        module.set_param("count", 2)
        self.assertEqual(module.get_param("count"), 2)
        with self.assertRaises(TypeError):
            module.set_param("count", "wrong")
        with self.assertRaises(KeyError):
            module.set_param("missing", 1)

        module.register_param("items", [])
        descriptor = module.params.type_of("items")
        module.set_param("items", [1, "two"])
        self.assertEqual(module.params.type_of("items"), descriptor)

        module.register_param("counts", [1, 2])
        with self.assertRaises(TypeError):
            module.set_param("counts", [1, "two"])

    def test_parameter_yaml_uses_the_core_format(self):
        module = Module()
        module.register_param("threshold", 1.0, "selection threshold")
        path = pathlib.Path(self.tempdir.name) / "params.yaml"
        path.write_text(
            "threshold:\n  type: double\n  value: 2\n  description: updated\n",
            encoding="utf-8",
        )
        module.set_param_from_yaml(path)
        self.assertEqual(module.get_param("threshold"), 2.0)
        self.assertIn("description: updated", module.params.dump_yaml())

    def test_output_transaction_commits_and_rolls_back(self):
        output_dir = pathlib.Path(self.tempdir.name) / "outputs"
        cache_dir = pathlib.Path(self.tempdir.name) / "cache"

        success = OutputModule()
        success.set_output_directory(output_dir)
        success.set_cache_directory(cache_dir)
        self.assertTrue(success.run().succeeded())
        self.assertEqual((output_dir / "result.txt").read_text(encoding="utf-8"), "new")
        manifest = json.loads(
            pathlib.Path(success.get_last_provenance_path()).read_text(encoding="utf-8")
        )
        self.assertEqual(manifest["artifacts"]["outputs"][0]["path"], "result.txt")
        self.assertEqual(len(manifest["artifacts"]["outputs"][0]["sha256"]), 64)

        (output_dir / "result.txt").write_text("old", encoding="utf-8")
        failure = OutputModule(base.ModulePhase.EXECUTE)
        failure.set_output_directory(output_dir)
        failure.set_cache_directory(cache_dir)
        result = failure.run()
        self.assertEqual(result.phase, base.ModulePhase.EXECUTE)
        self.assertEqual((output_dir / "result.txt").read_text(encoding="utf-8"), "old")
        self.assertFalse((output_dir / ".cascade-staging").exists())

        overlap = base.OutputTransaction()
        overlap.begin(output_dir, "overlap-test")
        overlap.stage("plots")
        with self.assertRaises(RuntimeError):
            overlap.stage("plots/detail.pdf")
        overlap.rollback()

        outside = pathlib.Path(self.tempdir.name) / "outside"
        outside.mkdir()
        (output_dir / "outside-link").symlink_to(outside, target_is_directory=True)
        escaped = base.OutputTransaction()
        escaped.begin(output_dir, "escape-test")
        with self.assertRaises(RuntimeError):
            escaped.stage("outside-link/result.txt")
        escaped.rollback()

    def test_skipped_result_allows_dependents(self):
        result = base.RunResult(
            base.ModuleStatus.SKIPPED,
            base.ModulePhase.CHECK,
            "snapshot already cached",
        )
        self.assertTrue(result.is_terminal())
        self.assertTrue(result.allows_dependents())

    def test_snapshot_caches_are_isolated_by_directory(self):
        output_dir = pathlib.Path(self.tempdir.name) / "outputs"
        cache_a = pathlib.Path(self.tempdir.name) / "cache-a"
        cache_b = pathlib.Path(self.tempdir.name) / "cache-b"

        def configured(cache_dir):
            module = Module()
            module.set_param("force_run", False)
            module.set_output_directory(output_dir)
            module.set_cache_directory(cache_dir)
            return module

        first_a = configured(cache_a)
        self.assertEqual(first_a.run().status, base.ModuleStatus.DONE)
        source_manifest = first_a.get_last_provenance_path()
        self.assertEqual(configured(cache_b).run().status, base.ModuleStatus.DONE)
        cached_a = configured(cache_a)
        self.assertEqual(cached_a.run().status, base.ModuleStatus.SKIPPED)
        cached_manifest = json.loads(
            pathlib.Path(cached_a.get_last_provenance_path()).read_text(
                encoding="utf-8"
            )
        )
        self.assertEqual(
            cached_manifest["execution"]["cache_source_manifest"], source_manifest
        )

    def test_sensitive_parameters_are_redacted(self):
        module = Module()
        module.register_param("api_token", "do-not-record")
        module.set_output_directory(pathlib.Path(self.tempdir.name) / "output")
        module.set_cache_directory(pathlib.Path(self.tempdir.name) / "cache")
        self.assertTrue(module.run().succeeded())
        manifest = json.loads(
            pathlib.Path(module.get_last_provenance_path()).read_text(
                encoding="utf-8"
            )
        )
        self.assertEqual(manifest["parameters"]["api_token"], "***")

    def test_prepared_external_result_is_adopted(self):
        module = Module()
        module.set_output_directory(pathlib.Path(self.tempdir.name) / "outputs")
        module.prepare_external_run()
        result = base.RunResult(base.ModuleStatus.DONE, base.ModulePhase.NONE, "")
        adopted = module.adopt_external_run_result(result)
        self.assertTrue(adopted.succeeded())

    def test_controller_rejects_non_plugin_module_isolation(self):
        module = Module()
        module.set_name("python-isolated")
        module.set_output_directory(pathlib.Path(self.tempdir.name) / "outputs")
        module.set_cache_directory(pathlib.Path(self.tempdir.name) / "cache")
        controller = sys.modules["cascade._cascade"].AMCM()
        controller.register_module_handle(module)
        with self.assertRaisesRegex(RuntimeError, "verified plugin"):
            controller.run_module_isolated(module)

    @unittest.skipUnless(hasattr(os, "fork"), "requires POSIX fork")
    def test_parent_recovers_output_after_isolated_crash(self):
        output_dir = pathlib.Path(self.tempdir.name) / "outputs"
        output_dir.mkdir()
        (output_dir / "result.txt").write_text("old", encoding="utf-8")

        module = CrashAfterPromotionModule()
        module.set_output_directory(output_dir)
        module.set_cache_directory(pathlib.Path(self.tempdir.name) / "cache")
        module.prepare_external_run()
        child = os.fork()
        if child == 0:
            module.run_prepared_external()
            os._exit(0)
        _, status = os.waitpid(child, 0)
        self.assertTrue(os.WIFSIGNALED(status))

        failed = base.RunResult(
            base.ModuleStatus.FAILED,
            base.ModulePhase.EXECUTE,
            "isolated crash",
            RuntimeError("isolated crash"),
        )
        module.adopt_external_run_result(failed)
        self.assertEqual((output_dir / "result.txt").read_text(encoding="utf-8"), "old")


if __name__ == "__main__":
    unittest.main()
