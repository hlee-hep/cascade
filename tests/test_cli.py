import argparse
import contextlib
import ctypes
import importlib
import importlib.machinery
import importlib.util
import io
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock


def _load_workspace_extension():
    build_root = pathlib.Path(__file__).parents[1] / "build"
    for library in (
        build_root / "utils" / "libutils.so",
        build_root / "ParamManager" / "libParamManager.so",
        build_root / "AnalysisManager" / "libAnalysisManager.so",
        build_root / "PlotManager" / "libPlotManager.so",
        build_root / "src" / "libAMCM.so",
    ):
        ctypes.CDLL(str(library), mode=ctypes.RTLD_GLOBAL)
    extension_path = build_root / "main" / "libCascade.so"
    loader = importlib.machinery.ExtensionFileLoader("cascade._cascade", str(extension_path))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    extension = importlib.util.module_from_spec(spec)
    loader.exec_module(extension)
    sys.modules["cascade._cascade"] = extension


_load_workspace_extension()


def _load_cli():
    path = pathlib.Path(__file__).parents[1] / "python" / "cascade"
    loader = importlib.machinery.SourceFileLoader("cascade_test_cli", str(path))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


cli = _load_cli()
from cascade_cli import common as cli_common
from cascade_cli import cache as cli_cache
from cascade_cli import execution as cli_execution
from cascade_cli import plugin as cli_plugin
from cascade_cli import parser as cli_parser
from cascade_cli import provenance as cli_provenance
from cascade_cli import system as cli_system

cli_main = importlib.import_module("cascade_cli.main")


class _FakeCacheManager:
    snapshots = []

    @classmethod
    def reset(cls):
        cls.snapshots = []

    @classmethod
    def add_hash(cls, module, snapshot_hash, directory, provenance=""):
        cls.snapshots.append(types.SimpleNamespace(
            module=module,
            hash=snapshot_hash,
            provenance=provenance,
            cache_file=str(pathlib.Path(directory) / f"{module}.yaml"),
        ))

    @classmethod
    def list_snapshots(cls, directory, module=""):
        return [entry for entry in cls.snapshots if not module or entry.module == module]

    @classmethod
    def is_hash_cached(cls, module, snapshot_hash, directory):
        return any(entry.module == module and entry.hash == snapshot_hash for entry in cls.snapshots)

    @classmethod
    def find_provenance(cls, module, snapshot_hash, directory):
        return next(
            (entry.provenance for entry in cls.snapshots if entry.module == module and entry.hash == snapshot_hash),
            "",
        )

    @classmethod
    def prune(cls, directory, module="", remove_all=False, dry_run=False):
        removed = [
            entry for entry in cls.snapshots
            if (not module or entry.module == module)
            and (remove_all or bool(entry.provenance and not os.path.isfile(entry.provenance)))
        ]
        if not dry_run:
            cls.snapshots = [entry for entry in cls.snapshots if entry not in removed]
        return removed


class _FakeHandle:
    def __init__(self, name):
        self._name = name
        self.output = None
        self.cache = None
        self.params = {}

    def name(self):
        return self._name

    def set_output_directory(self, path):
        self.output = path

    def set_cache_directory(self, path):
        self.cache = path

    def set_param(self, key, value):
        self.params[key] = value

    def get_run_id(self):
        return "replayed-run"

    def get_last_provenance_path(self):
        return "/tmp/replayed-run.json"


class _FakeResult:
    def __init__(self, nodes=None):
        self.nodes = nodes or []

    def succeeded(self):
        return True

    def failed(self):
        return False


class _FakeModuleResult:
    status = types.SimpleNamespace(name="Done")
    phase = types.SimpleNamespace(name="None_")
    message = ""
    cache_decision = "hit"
    cache_reason = "snapshot and recorded outputs matched"

    def succeeded(self):
        return True

    def allows_dependents(self):
        return True


class _FakeDag:
    def __init__(self):
        self.dot = None
        self.validation_error = None

    def validate(self):
        if self.validation_error:
            raise self.validation_error

    def dump_dot(self, path):
        self.dot = path


class _FakeController:
    def __init__(self):
        self.handles = {}
        self.nodes = []
        self.links = []
        self.fail_fast = None
        self.provenance = None
        self.last_workflow_provenance_path = ""
        self.dag = _FakeDag()
        self.module_run = None

    def register_module(self, module, name):
        handle = _FakeHandle(name)
        self.handles[name] = (module, handle)
        return handle

    def add_module_to_dag(self, name, dependencies, isolated=False):
        self.nodes.append((name, dependencies, isolated))

    def link_dag_parameter(self, source, source_param, target, target_param):
        self.links.append((source, source_param, target, target_param))

    def run_dag(self, fail_fast=True, provenance_path=None):
        self.fail_fast = fail_fast
        self.provenance = provenance_path
        self.last_workflow_provenance_path = provenance_path or ""
        nodes = [
            types.SimpleNamespace(
                name=name,
                status=types.SimpleNamespace(name="Succeeded"),
                message="",
            )
            for name, _, _ in self.nodes
        ]
        return _FakeResult(nodes)

    def run_module(self, name, isolated=False):
        self.module_run = (name, isolated)
        return _FakeModuleResult()

    def get_dag(self):
        return self.dag


class CliTests(unittest.TestCase):
    @staticmethod
    def _module_manifest(run_id="run-one", threshold=10):
        return {
            "schema": "cascade.module-run",
            "schema_version": 1,
            "run_id": run_id,
            "module": {
                "instance": "analysis",
                "name": "AnalysisModule",
                "metadata": {"name": "AnalysisModule", "version": "1.0", "summary": "", "tags": []},
            },
            "runtime": {"language": "python", "cascade_version": "0.3.0"},
            "identity": {"code_hash": "code", "snapshot_hash": f"snapshot-{threshold}"},
            "parameters": {"threshold": threshold},
            "timing": {"started_at": "2026-08-02T01:00:00Z", "finished_at": "2026-08-02T01:00:01Z"},
            "directories": {"output": "/tmp/output", "cache": "/tmp/cache"},
            "execution": {
                "isolated": True,
                "cache_hit": False,
                "dry_run": False,
                "cache_source_manifest": None,
            },
            "result": {"status": "Done", "phase": "None", "message": ""},
            "artifacts": {"inputs": [], "outputs": []},
            "manifest_path": f"/old/location/{run_id}.json",
        }

    def test_key_value_parsing(self):
        self.assertEqual(cli_common._parse_kv("enabled=true"), ("enabled", True))
        self.assertEqual(cli_common._parse_kv("count=4"), ("count", 4))
        self.assertEqual(cli_common._parse_kv('items=["a", 2]'), ("items", ["a", 2]))
        with self.assertRaises(argparse.ArgumentTypeError):
            cli_common._parse_kv("=missing")

    def test_logging_uses_standard_stderr_format_for_every_line(self):
        stdout = io.StringIO()
        stderr = io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            cli_common._log("WARN", "first\nsecond", "PLUGIN")
            cli_common._log("OK", "runtime ready", "DOCTOR")
        self.assertEqual(stdout.getvalue(), "")
        self.assertEqual(
            stderr.getvalue(),
            "[WARNING] [PLUGIN] first\n"
            "[WARNING] [PLUGIN] second\n"
            "[INFO] [DOCTOR] OK: runtime ready\n",
        )

    def test_entrypoint_exports_main(self):
        self.assertTrue(callable(cli.main))

    def test_banner_is_limited_to_top_level_tty_usage(self):
        with mock.patch.object(sys.stdout, "isatty", return_value=True):
            self.assertTrue(cli_main._show_banner([]))
            self.assertTrue(cli_main._show_banner(["--help"]))
            self.assertFalse(cli_main._show_banner(["--version"]))
            self.assertFalse(cli_main._show_banner(["module", "list"]))
        with mock.patch.object(sys.stdout, "isatty", return_value=False):
            self.assertFalse(cli_main._show_banner([]))

    def test_no_argument_entrypoint_prints_banner_and_help_without_running_a_command(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output), mock.patch.object(output, "isatty", return_value=True), \
             mock.patch.dict(os.environ, {"NO_COLOR": "1"}, clear=False):
            cli_main.main([])
        rendered = output.getvalue()
        self.assertIn("Composable Analysis with Secure Caching And DAG Execution", rendered)
        self.assertIn("usage: cascade", rendered)
        self.assertNotIn("\033[", rendered)

    def test_signed_policy_is_a_global_strengthening_option(self):
        args = cli_parser.build_parser().parse_args(["--require-signed", "module", "list"])
        self.assertTrue(args.require_signed)
        self.assertIs(args.func, cli_execution.cmd_module_list)

    def test_module_run_explains_cache_and_restores_runtime_options(self):
        controller = _FakeController()
        args = cli_parser.build_parser().parse_args(
            [
                "module", "run", "AnalysisModule", "--name", "analysis",
                "--explain-cache", "--input-hash", "full", "--output-hash", "metadata",
                "--timeout", "12",
            ]
        )
        output = io.StringIO()
        previous_input = os.environ.get("CASCADE_INPUT_HASH_MODE")
        with mock.patch.object(cli_execution, "_load_controller", return_value=controller), \
                contextlib.redirect_stdout(output):
            cli_execution.cmd_module_run(args)
        self.assertIn("Cache hit: snapshot and recorded outputs matched", output.getvalue())
        self.assertEqual(os.environ.get("CASCADE_INPUT_HASH_MODE"), previous_input)

    def test_runtime_doctor_reports_resolved_workers_and_policies(self):
        build_directory = pathlib.Path(__file__).parents[1] / "build"
        with tempfile.TemporaryDirectory(dir=build_directory) as directory:
            root = pathlib.Path(directory)
            cpp_worker = root / "cascade-worker"
            python_worker = root / "cascade-python-worker"
            runtime = root / "runtime"
            runtime.mkdir()
            for worker in (cpp_worker, python_worker):
                worker.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
                worker.chmod(0o700)
            environment = {
                "CASCADE_CPP_WORKER": str(cpp_worker),
                "CASCADE_PYTHON_WORKER": str(python_worker),
                "CASCADE_PYTHON_RUNTIME_DIR": str(runtime),
                "CASCADE_INPUT_HASH_MODE": "auto",
                "CASCADE_PROVENANCE_HASH_MODE": "full",
            }
            output = io.StringIO()
            with mock.patch.dict(os.environ, environment, clear=False), contextlib.redirect_stdout(output):
                cli_system.cmd_doctor_runtime(types.SimpleNamespace(json=True))
            payload = json.loads(output.getvalue())
            self.assertEqual(payload["runtime"]["input_hash"], "auto")
            self.assertTrue(all(check["status"] == "OK" for check in payload["checks"]))

    def test_dag_progress_and_runtime_options_are_exposed(self):
        args = cli_parser.build_parser().parse_args(
            ["dag", "run", "workflow.yaml", "--progress", "--workers", "3", "--input-hash", "full"]
        )
        self.assertTrue(args.progress)
        self.assertEqual(args.workers, 3)
        self.assertEqual(args.input_hash, "full")
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                cli_parser.build_parser().parse_args(
                    ["dag", "run", "workflow.yaml", "--workers", "1.5"]
                )
            with self.assertRaises(SystemExit):
                cli_parser.build_parser().parse_args(
                    ["module", "run", "AnalysisModule", "--timeout", "nan"]
                )

    def test_cache_list_explain_and_prune_missing_provenance(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            existing = root / "run.json"
            existing.write_text("{}", encoding="utf-8")
            _FakeCacheManager.reset()
            _FakeCacheManager.add_hash("analysis", "present", str(root), str(existing))
            _FakeCacheManager.add_hash("analysis", "stale", str(root), str(root / "missing.json"))

            listed = types.SimpleNamespace(
                cache_directory=str(root), module=None, json=True
            )
            output = io.StringIO()
            with mock.patch.object(cli_cache, "_cache_manager", return_value=_FakeCacheManager), \
                    contextlib.redirect_stdout(output):
                cli_cache.cmd_cache_list(listed)
            payload = json.loads(output.getvalue())
            self.assertEqual(payload["count"], 2)
            self.assertEqual(
                [entry["hash"] for entry in payload["snapshots"]],
                ["present", "stale"],
            )

            explained = types.SimpleNamespace(
                cache_directory=str(root), module="analysis", hash="present", json=True
            )
            output = io.StringIO()
            with mock.patch.object(cli_cache, "_cache_manager", return_value=_FakeCacheManager), \
                    contextlib.redirect_stdout(output):
                cli_cache.cmd_cache_explain(explained)
            self.assertTrue(json.loads(output.getvalue())["cached"])

            pruned = types.SimpleNamespace(
                cache_directory=str(root), module=None, all=False, dry_run=False, json=True
            )
            output = io.StringIO()
            with mock.patch.object(cli_cache, "_cache_manager", return_value=_FakeCacheManager), \
                    contextlib.redirect_stdout(output):
                cli_cache.cmd_cache_prune(pruned)
            self.assertEqual(json.loads(output.getvalue())["removed_count"], 1)
            remaining = _FakeCacheManager.list_snapshots(str(root))
            self.assertEqual([entry.hash for entry in remaining], ["present"])

    def test_cache_prune_dry_run_does_not_modify_cache(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            cache_file = root / "analysis.yaml"
            _FakeCacheManager.reset()
            _FakeCacheManager.add_hash("analysis", "first", str(root), "")
            before = list(_FakeCacheManager.snapshots)
            args = types.SimpleNamespace(
                cache_directory=str(root), module="analysis", all=True, dry_run=True, json=True
            )
            with mock.patch.object(cli_cache, "_cache_manager", return_value=_FakeCacheManager), \
                    contextlib.redirect_stdout(io.StringIO()):
                cli_cache.cmd_cache_prune(args)
            self.assertEqual(_FakeCacheManager.snapshots, before)

    def test_cache_prune_filters_by_module(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            _FakeCacheManager.reset()
            _FakeCacheManager.add_hash("first", "one", str(root), "")
            _FakeCacheManager.add_hash("second", "two", str(root), "")

            args = types.SimpleNamespace(
                cache_directory=str(root), module="first", all=True, dry_run=False, json=True
            )
            with mock.patch.object(cli_cache, "_cache_manager", return_value=_FakeCacheManager), \
                    contextlib.redirect_stdout(io.StringIO()):
                cli_cache.cmd_cache_prune(args)
            self.assertEqual(
                [(entry.module, entry.hash) for entry in _FakeCacheManager.list_snapshots(str(root))],
                [("second", "two")],
            )

    def test_root_arguments_are_escaped(self):
        with tempfile.TemporaryDirectory() as directory:
            macro = pathlib.Path(directory) / "Macro.C"
            macro.write_text("", encoding="utf-8")
            completed = types.SimpleNamespace(returncode=0)
            with mock.patch.object(cli_system.subprocess, "run", return_value=completed) as run:
                cli_system._root_invoke(
                    str(macro),
                    None,
                    ['quote"value', r"slash\value"],
                    use_plus=True,
                )
            command = run.call_args.args[0]
            expected_arguments = ",".join(
                json.dumps(value, ensure_ascii=False)
                for value in ['quote"value', r"slash\value"]
            )
            self.assertEqual(command[-1], f"{macro}+({expected_arguments})")

    def test_plugin_roots_visit_each_package_once(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "one").mkdir()
            (root / "two").mkdir()
            (root / "ignored.txt").write_text("", encoding="utf-8")
            with mock.patch.object(
                cli_plugin,
                "_doctor_plugin_package",
                return_value=(0, []),
            ) as doctor:
                cli_plugin._doctor_plugin_dir(str(root), "python", [])
            visited = [pathlib.Path(call.args[0]).name for call in doctor.call_args_list]
            self.assertEqual(visited, ["one", "two"])

    def test_unsigned_plugin_is_verified_and_strict_policy_rejects_it(self):
        with tempfile.TemporaryDirectory() as directory:
            package = pathlib.Path(directory) / "local-package"
            package.mkdir()
            source = package / "local_module.py"
            source.write_text(
                "from cascade.pymodule.base_module import base_module\n"
                "class LocalModule(base_module):\n"
                "    pass\n",
                encoding="utf-8",
            )
            manifest = {
                "schema": 2,
                "package": package.name,
                "modules": [{
                    "name": "local_module",
                    "language": "python",
                    "path": source.name,
                    "sha256": cli_plugin._sha256_file(str(source)),
                    "classes": ["LocalModule"],
                }],
            }
            (package / "plugin_manifest.json").write_text(json.dumps(manifest), encoding="utf-8")

            reports = []
            verified_package = types.SimpleNamespace(
                package=package.name,
                trust="Verified",
                trusted_key_path="",
                artifacts=[types.SimpleNamespace(
                    name="local_module",
                    classes=["LocalModule"],
                )],
            )
            with contextlib.redirect_stdout(io.StringIO()), mock.patch.object(
                cli_plugin,
                "_core_verify_package",
                return_value=verified_package,
            ):
                errors, names = cli_plugin._doctor_plugin_package(
                    str(package), "python", [], reports=reports
                )
            self.assertEqual(errors, 0)
            self.assertEqual(names, ["LocalModule"])
            self.assertEqual(reports[0]["trust"], "VERIFIED")

            strict_reports = []
            with contextlib.redirect_stdout(io.StringIO()), mock.patch.object(
                cli_plugin,
                "_core_verify_package",
                side_effect=RuntimeError("plugin package requires a trusted signature"),
            ):
                strict_errors, _ = cli_plugin._doctor_plugin_package(
                    str(package),
                    "python",
                    [],
                    require_signed=True,
                    reports=strict_reports,
                )
            self.assertEqual(strict_errors, 1)

    def test_plugin_verifier_does_not_reuse_another_packages_key(self):
        captured = {}

        class Verifier:
            @staticmethod
            def verify_package(*args):
                captured["trusted_key"] = args[-1]
                return types.SimpleNamespace()

        extension = types.ModuleType("cascade._cascade")
        extension.PluginTrustPolicy = types.SimpleNamespace(
            Verified="verified", RequireSigned="require-signed"
        )
        extension.PluginVerifier = Verifier
        cascade_module = types.ModuleType("cascade")
        with tempfile.TemporaryDirectory() as directory, mock.patch.dict(
            sys.modules,
            {"cascade": cascade_module, "cascade._cascade": extension},
        ):
            other_key = str(pathlib.Path(directory) / "0000-other-package.pem")
            cli_plugin._core_verify_package(
                str(pathlib.Path(directory) / "target-package"),
                "python",
                [other_key],
                False,
            )
        self.assertEqual(captured["trusted_key"], "")

    def test_plugin_path_parser_and_commands_persist_prefix(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            config = root / "config.json"
            prefix = root / "plugins"
            prefix.mkdir()
            with mock.patch.dict(os.environ, {"CASCADE_CONFIG_FILE": str(config)}, clear=False):
                args = cli_parser.build_parser().parse_args(["plugin", "path", "add", str(prefix), "--json"])
                with contextlib.redirect_stdout(io.StringIO()):
                    args.func(args)
                self.assertEqual(cli_plugin.configured_plugin_prefixes(), [str(prefix.resolve())])

                listed = cli_parser.build_parser().parse_args(["plugin", "path", "list", "--json"])
                output = io.StringIO()
                with contextlib.redirect_stdout(output):
                    listed.func(listed)
                payload = json.loads(output.getvalue())
                self.assertEqual(payload["plugin_prefixes"][0]["path"], str(prefix.resolve()))

                removed = cli_parser.build_parser().parse_args(["plugin", "path", "remove", str(prefix), "--json"])
                with contextlib.redirect_stdout(io.StringIO()):
                    removed.func(removed)
                self.assertEqual(cli_plugin.configured_plugin_prefixes(), [])

    def test_publish_transaction_can_restore_previous_package(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            stage = root / "stage"
            target = root / "target"
            package = "example"
            staged_package = stage / "lib" / "cascade" / "pyplugin" / package
            target_package = target / "lib" / "cascade" / "pyplugin" / package
            staged_package.mkdir(parents=True)
            target_package.mkdir(parents=True)
            (staged_package / "module.py").write_text("new", encoding="utf-8")
            (target_package / "module.py").write_text("old", encoding="utf-8")

            operations = cli_plugin._publish_staged_plugin(str(stage), str(target), package)
            self.assertEqual((target_package / "module.py").read_text(encoding="utf-8"), "new")
            cli_plugin._restore_publish_operations(operations)
            self.assertEqual((target_package / "module.py").read_text(encoding="utf-8"), "old")

    def test_strict_install_does_not_trust_a_key_created_by_the_build(self):
        if shutil.which("openssl") is None:
            self.skipTest("openssl is required")
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            stage = root / "stage"
            target = root / "target"
            package_name = "self-signed"
            package = stage / "lib" / "cascade" / "pyplugin" / package_name
            trust_store = stage / "share" / "cascade" / "trusted_keys"
            package.mkdir(parents=True)
            trust_store.mkdir(parents=True)
            source = package / "module.py"
            source.write_text("class SelfSignedModule:\n    pass\n", encoding="utf-8")
            manifest = package / "plugin_manifest.json"
            manifest.write_text(json.dumps({
                "schema": 2,
                "package": package_name,
                "modules": [{
                    "name": "module",
                    "language": "python",
                    "path": source.name,
                    "sha256": cli_plugin._sha256_file(str(source)),
                    "classes": ["SelfSignedModule"],
                }],
            }), encoding="utf-8")
            private_key = root / "private.pem"
            staged_key = trust_store / f"{package_name}.pem"
            subprocess.check_call(["openssl", "genpkey", "-algorithm", "ED25519", "-out", str(private_key)])
            subprocess.check_call(["openssl", "pkey", "-in", str(private_key), "-pubout", "-out", str(staged_key)])
            subprocess.check_call([
                "openssl", "pkeyutl", "-sign", "-inkey", str(private_key), "-rawin",
                "-in", str(manifest), "-out", str(manifest) + ".sig",
            ])

            snapshots = cli_plugin._snapshot_trusted_keys(str(target), None)
            with self.assertRaisesRegex(RuntimeError, "verification failed"), mock.patch.object(
                cli_plugin,
                "_core_verify_package",
                side_effect=RuntimeError("package-bound trusted key is missing"),
            ):
                cli_plugin._verify_staged_plugin(
                    str(stage), package_name, True, snapshots, quiet=True
                )

    def test_staged_plugin_symlinks_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            outside = root / "outside"
            outside.mkdir()
            stage = root / "stage"
            package = stage / "lib" / "cascade" / "pyplugin" / "linked"
            package.parent.mkdir(parents=True)
            package.symlink_to(outside, target_is_directory=True)
            with self.assertRaisesRegex(RuntimeError, "real directory"):
                cli_plugin._verify_staged_plugin(str(stage), "linked", False, [], quiet=True)

    def test_rollback_attempts_every_operation_and_preserves_backups(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            first_destination = root / "first"
            second_destination = root / "second"
            first_backup = root / "backup-first"
            second_backup = root / "backup-second"
            first_destination.write_text("new-first", encoding="utf-8")
            second_destination.write_text("new-second", encoding="utf-8")
            first_backup.write_text("old-first", encoding="utf-8")
            second_backup.write_text("old-second", encoding="utf-8")
            operations = [
                {"label": "first", "destination": str(first_destination), "backup": str(first_backup), "had_destination": True, "published": True},
                {"label": "second", "destination": str(second_destination), "backup": str(second_backup), "had_destination": True, "published": True},
            ]
            real_remove = cli_plugin._remove_path

            def fail_first(path):
                if path == str(first_destination):
                    raise OSError("injected removal failure")
                return real_remove(path)

            with mock.patch.object(cli_plugin, "_remove_path", side_effect=fail_first):
                with self.assertRaisesRegex(RuntimeError, "first remove"):
                    cli_plugin._restore_publish_operations(operations)
            self.assertEqual(second_destination.read_text(encoding="utf-8"), "old-second")
            self.assertTrue(first_backup.exists())

    def test_prospective_install_rejects_cross_package_and_cross_language_duplicates(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            stage = root / "stage"
            target = root / "target"

            def write_manifest(package_dir, package_name, language, module_name):
                package_dir.mkdir(parents=True)
                (package_dir / "plugin_manifest.json").write_text(json.dumps({
                    "schema": 2,
                    "package": package_name,
                    "modules": [{
                        "name": module_name,
                        "language": language,
                        "path": "module.py" if language == "python" else "Module.so",
                        "sha256": "unused",
                        "classes": [module_name] if language == "python" else [],
                    }],
                }), encoding="utf-8")

            write_manifest(
                target / "lib" / "cascade" / "pyplugin" / "existing",
                "existing", "python", "DuplicateModule",
            )
            write_manifest(
                stage / "lib" / "cascade" / "plugin" / "incoming",
                "incoming", "cpp", "DuplicateModule",
            )
            layout = cli_plugin.plugin_layout(str(target))
            with mock.patch.object(cli_plugin, "_runtime_plugin_layouts", return_value=[{
                **layout, "source": "test",
            }]):
                with self.assertRaisesRegex(RuntimeError, "DuplicateModule"):
                    cli_plugin._reject_prospective_duplicates(str(stage), str(target), "incoming")

    def test_plugin_install_publishes_then_registers_prefix(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            source = root / "source-plugin"
            target = root / "target"
            stage = target / ".cascade-plugin-stage-test"
            config = root / "config.json"
            source.mkdir()
            target.mkdir()
            (source / "python").mkdir()
            (source / "python" / "example_module.py").write_text(
                "class ExampleModule:\n    pass\n", encoding="utf-8"
            )
            staged_package = stage / "lib" / "cascade" / "pyplugin" / source.name
            staged_package.mkdir(parents=True)
            (staged_package / "module.py").write_text("installed", encoding="utf-8")
            args = types.SimpleNamespace(
                source=str(source),
                prefix=str(target),
                package=None,
                private_key=None,
                public_key=None,
                scons="scons",
                jobs=2,
                json=True,
                require_signed=False,
            )
            completed = types.SimpleNamespace(returncode=0, stdout="", stderr="")
            with mock.patch.dict(os.environ, {"CASCADE_CONFIG_FILE": str(config)}, clear=False), \
                    mock.patch.object(cli_plugin.tempfile, "mkdtemp", return_value=str(stage)), \
                    mock.patch.object(cli_plugin.subprocess, "run", return_value=completed) as run, \
                    mock.patch.object(cli_plugin, "_verify_staged_plugin", return_value={"packages": []}):
                with contextlib.redirect_stdout(io.StringIO()):
                    cli_plugin.cmd_plugin_install(args)

            installed = target / "lib" / "cascade" / "pyplugin" / source.name / "module.py"
            self.assertEqual(installed.read_text(encoding="utf-8"), "installed")
            config_document = json.loads(config.read_text(encoding="utf-8"))
            self.assertEqual(
                config_document["plugin_prefixes"],
                [{"enabled": True, "path": str(target.resolve())}],
            )
            build_environment = run.call_args.kwargs["env"]
            self.assertEqual(build_environment["CASCADE_PYPLUGIN_DIR"], str(stage / "lib" / "cascade" / "pyplugin"))
            command = run.call_args.args[0]
            self.assertEqual(command[0], "scons")
            self.assertEqual(command[1], "-f")
            self.assertTrue(command[2].endswith("scripts/plugin_sconstruct"))
            self.assertEqual(build_environment["CASCADE_PLUGIN_ROOT_MODULES"], "")
            self.assertEqual(json.loads(build_environment["CASCADE_PLUGIN_CLASS_MAP"]), {})

    def test_convention_plugin_configuration_declares_root_modules_and_class_map(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory)
            (source / "include").mkdir()
            (source / "src").mkdir()
            (source / "include" / "EventModule.hh").write_text("", encoding="utf-8")
            (source / "src" / "EventModule.cc").write_text("", encoding="utf-8")
            (source / "cascade-plugin.json").write_text(json.dumps({
                "schema_version": 1,
                "root_modules": ["EventModule"],
                "class_map": {"EventModule": "experiment::EventModule"},
            }), encoding="utf-8")

            with mock.patch.object(cli_plugin, "_plugin_build_template", return_value="/sdk/plugin_sconstruct"):
                build = cli_plugin._load_convention_build(str(source))

            self.assertEqual(build, {
                "template": "/sdk/plugin_sconstruct",
                "root_modules": ["EventModule"],
                "class_map": {"EventModule": "experiment::EventModule"},
                "metadata": {},
            })

    def test_convention_plugin_rejects_unmatched_cpp_sources(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory)
            (source / "include").mkdir()
            (source / "include" / "EventModule.hh").write_text("", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "missing src/\\*\\.cc for EventModule"):
                cli_plugin._load_convention_build(str(source))

    def test_convention_plugin_rejects_removed_placeholders(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory)
            (source / "python").mkdir()
            (source / "python" / "example_module.py").write_text(
                'code_version_hash = "@VERSION_HASH@"\n', encoding="utf-8"
            )
            with self.assertRaisesRegex(ValueError, "removed build placeholders"):
                cli_plugin._load_convention_build(str(source))

    def test_plugin_install_rejects_package_owned_sconstruct(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "legacy-plugin"
            source.mkdir()
            (source / "SConstruct").write_text("", encoding="utf-8")
            args = types.SimpleNamespace(source=str(source))
            with self.assertRaisesRegex(ValueError, "SConstruct files are no longer supported"):
                cli_plugin.cmd_plugin_install(args)

    def test_plugin_install_restores_previous_package_if_config_update_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            source = root / "rollback-plugin"
            target = root / "target"
            stage = target / ".cascade-plugin-stage-test"
            source.mkdir()
            target.mkdir()
            (source / "python").mkdir()
            (source / "python" / "rollback_module.py").write_text(
                "class RollbackModule:\n    pass\n", encoding="utf-8"
            )
            staged_package = stage / "lib" / "cascade" / "pyplugin" / source.name
            installed_package = target / "lib" / "cascade" / "pyplugin" / source.name
            staged_package.mkdir(parents=True)
            installed_package.mkdir(parents=True)
            (staged_package / "module.py").write_text("new", encoding="utf-8")
            (installed_package / "module.py").write_text("old", encoding="utf-8")
            args = types.SimpleNamespace(
                source=str(source),
                prefix=str(target),
                package=None,
                private_key=None,
                public_key=None,
                scons="scons",
                jobs=2,
                json=True,
                require_signed=False,
            )
            completed = types.SimpleNamespace(returncode=0, stdout="", stderr="")
            with mock.patch.object(cli_plugin.tempfile, "mkdtemp", return_value=str(stage)), \
                    mock.patch.object(cli_plugin.subprocess, "run", return_value=completed), \
                    mock.patch.object(cli_plugin, "_verify_staged_plugin", return_value={"packages": []}), \
                    mock.patch.object(cli_plugin, "add_plugin_prefix", side_effect=OSError("config failed")):
                with self.assertRaises(OSError), contextlib.redirect_stdout(io.StringIO()):
                    cli_plugin.cmd_plugin_install(args)
            self.assertEqual((installed_package / "module.py").read_text(encoding="utf-8"), "old")

    def test_dag_workflow_wires_modules_and_links(self):
        workflow = {
            "schema_version": 1,
            "output_directory": "output",
            "cache_directory": "cache",
            "fail_fast": False,
            "dot": "output/workflow.dot",
            "provenance": "output/workflow-provenance.json",
            "modules": [
                {
                    "module": "ProducerModule",
                    "name": "producer",
                    "params": {"force_run": True, "value": 7},
                },
                {
                    "module": "ConsumerModule",
                    "name": "consumer",
                    "dependencies": ["producer"],
                    "isolated": True,
                },
            ],
            "links": [
                {"from": "producer.value", "to": "consumer.input_value"},
            ],
        }
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "workflow.json"
            path.write_text(json.dumps(workflow), encoding="utf-8")
            controller = _FakeController()
            args = types.SimpleNamespace(
                workflow=str(path),
                fail_fast=None,
                dot=None,
                json=False,
            )
            with mock.patch.object(cli_execution, "_load_controller", return_value=controller):
                cli_execution.cmd_dag_run(args)

            self.assertEqual(
                controller.nodes,
                [
                    ("producer", [], False),
                    ("consumer", ["producer"], True),
                ],
            )
            self.assertEqual(
                controller.links,
                [("producer", "value", "consumer", "input_value")],
            )
            self.assertFalse(controller.fail_fast)
            self.assertEqual(
                controller.provenance,
                str(pathlib.Path(directory) / "output" / "workflow-provenance.json"),
            )
            self.assertEqual(
                controller.handles["producer"][1].params,
                {"force_run": True, "value": 7},
            )
            self.assertEqual(
                controller.dag.dot,
                str(pathlib.Path(directory) / "output" / "workflow.dot"),
            )

    def test_dag_workflow_rejects_unknown_fields(self):
        workflow = {
            "schema_version": 1,
            "modules": [{"module": "Module", "name": "module"}],
            "typo": True,
        }
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "workflow.json"
            path.write_text(json.dumps(workflow), encoding="utf-8")
            args = types.SimpleNamespace(
                workflow=str(path),
                fail_fast=None,
                dot=None,
                json=False,
            )
            with self.assertRaises(ValueError):
                cli_execution.cmd_dag_run(args)

    def test_dag_validate_configures_without_execution(self):
        workflow = {
            "schema_version": 1,
            "modules": [
                {"module": "Producer", "name": "producer", "params": {"value": 1}},
                {"module": "Consumer", "name": "consumer", "dependencies": ["producer"]},
            ],
            "links": [{"from": "producer.value", "to": "consumer.input"}],
        }
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "workflow.json"
            path.write_text(json.dumps(workflow), encoding="utf-8")
            controller = _FakeController()
            args = types.SimpleNamespace(workflow=str(path), json=False, require_signed=False)
            output = io.StringIO()
            with mock.patch.object(cli_execution, "_load_controller", return_value=controller), \
                    contextlib.redirect_stdout(output):
                cli_execution.cmd_dag_validate(args)
            self.assertIn("Valid workflow: 2 module(s), 1 parameter link(s).", output.getvalue())
            self.assertIsNone(controller.fail_fast)

    def test_dag_validate_rejects_cycles_before_loading_controller(self):
        workflow = {
            "schema_version": 1,
            "modules": [
                {"module": "First", "name": "first", "dependencies": ["second"]},
                {"module": "Second", "name": "second", "dependencies": ["first"]},
            ],
        }
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "workflow.json"
            path.write_text(json.dumps(workflow), encoding="utf-8")
            args = types.SimpleNamespace(workflow=str(path), json=False, require_signed=False)
            controller = _FakeController()
            controller.dag.validation_error = RuntimeError("Cycle detected at DAG node: first")
            with mock.patch.object(cli_execution, "_load_controller", return_value=controller) as load_controller, \
                    self.assertRaisesRegex(RuntimeError, "Cycle detected"):
                cli_execution.cmd_dag_validate(args)
            load_controller.assert_called_once()

    def test_history_discovers_and_sorts_provenance(self):
        with tempfile.TemporaryDirectory() as directory:
            provenance = pathlib.Path(directory) / "provenance" / "modules"
            provenance.mkdir(parents=True)
            older = self._module_manifest("run-older", 10)
            newer = self._module_manifest("run-newer", 20)
            newer["timing"]["started_at"] = "2026-08-02T02:00:00Z"
            (provenance / "older.json").write_text(json.dumps(older), encoding="utf-8")
            (provenance / "newer.json").write_text(json.dumps(newer), encoding="utf-8")
            args = types.SimpleNamespace(root=[directory], kind="module", limit=1, json=True)
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                cli_provenance.cmd_history(args)
            payload = json.loads(output.getvalue())
            self.assertEqual([entry["run_id"] for entry in payload["runs"]], ["run-newer"])

    def test_diff_ignores_run_identity_and_reports_parameter_changes(self):
        before = self._module_manifest("run-before", 10)
        after = self._module_manifest("run-after", 20)
        changes = cli_provenance._provenance_changes(
            cli_provenance._stable_provenance(before),
            cli_provenance._stable_provenance(after),
        )
        paths = {change["path"] for change in changes}
        self.assertIn("parameters.threshold", paths)
        self.assertIn("identity.snapshot_hash", paths)
        self.assertNotIn("run_id", paths)
        self.assertFalse(any(path.startswith("timing") for path in paths))

    def test_replay_restores_parameters_directories_and_isolation(self):
        with tempfile.TemporaryDirectory() as directory:
            manifest_path = pathlib.Path(directory) / "module.json"
            manifest_path.write_text(json.dumps(self._module_manifest()), encoding="utf-8")
            controller = _FakeController()
            args = types.SimpleNamespace(
                run=str(manifest_path),
                root=None,
                name=None,
                set=["threshold=25"],
                output_directory=None,
                cache_directory=None,
                isolated=None,
                json=False,
            )
            with mock.patch.object(cli_provenance, "_load_controller", return_value=controller):
                with contextlib.redirect_stdout(io.StringIO()):
                    cli_provenance.cmd_replay(args)
            module_name, handle = controller.handles["analysis"]
            self.assertEqual(module_name, "AnalysisModule")
            self.assertEqual(handle.params, {"threshold": 25})
            self.assertEqual(handle.output, "/tmp/output")
            self.assertEqual(handle.cache, "/tmp/cache")
            self.assertEqual(controller.module_run, ("analysis", True))

    def test_replay_requires_redacted_parameters(self):
        with tempfile.TemporaryDirectory() as directory:
            manifest = self._module_manifest()
            manifest["parameters"]["api_token"] = "***"
            manifest_path = pathlib.Path(directory) / "module.json"
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            args = types.SimpleNamespace(
                run=str(manifest_path),
                root=None,
                name=None,
                set=[],
                output_directory=None,
                cache_directory=None,
                isolated=None,
                json=False,
            )
            with self.assertRaisesRegex(ValueError, "api_token"):
                cli_provenance.cmd_replay(args)


if __name__ == "__main__":
    unittest.main()
