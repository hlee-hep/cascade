import argparse
import contextlib
import importlib.machinery
import importlib.util
import io
import json
import os
import pathlib
import tempfile
import types
import unittest
from unittest import mock


def _load_cli():
    path = pathlib.Path(__file__).parents[1] / "python" / "cascade"
    loader = importlib.machinery.SourceFileLoader("cascade_test_cli", str(path))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


cli = _load_cli()


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

    def succeeded(self):
        return True

    def allows_dependents(self):
        return True


class _FakeDag:
    def __init__(self):
        self.dot = None

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
        self.assertEqual(cli._parse_kv("enabled=true"), ("enabled", True))
        self.assertEqual(cli._parse_kv("count=4"), ("count", 4))
        self.assertEqual(cli._parse_kv('items=["a", 2]'), ("items", ["a", 2]))
        with self.assertRaises(argparse.ArgumentTypeError):
            cli._parse_kv("=missing")

    def test_signed_policy_is_a_global_strengthening_option(self):
        args = cli.build_parser().parse_args(["--require-signed", "module", "list"])
        self.assertTrue(args.require_signed)
        self.assertIs(args.func, cli.cmd_module_list)

    def test_root_arguments_are_escaped(self):
        with tempfile.TemporaryDirectory() as directory:
            macro = pathlib.Path(directory) / "Macro.C"
            macro.write_text("", encoding="utf-8")
            completed = types.SimpleNamespace(returncode=0)
            with mock.patch.object(cli.subprocess, "run", return_value=completed) as run:
                cli._root_invoke(
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
                cli,
                "_doctor_plugin_package",
                return_value=(0, []),
            ) as doctor:
                cli._doctor_plugin_dir(str(root), "python", False, [])
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
                    "sha256": cli._sha256_file(str(source)),
                    "classes": ["LocalModule"],
                }],
            }
            (package / "plugin_manifest.json").write_text(json.dumps(manifest), encoding="utf-8")

            reports = []
            with contextlib.redirect_stdout(io.StringIO()):
                errors, names = cli._doctor_plugin_package(
                    str(package), "python", False, [], reports=reports
                )
            self.assertEqual(errors, 0)
            self.assertEqual(names, ["LocalModule"])
            self.assertEqual(reports[0]["trust"], "VERIFIED")

            strict_reports = []
            with contextlib.redirect_stdout(io.StringIO()):
                strict_errors, _ = cli._doctor_plugin_package(
                    str(package),
                    "python",
                    False,
                    [],
                    require_signed=True,
                    reports=strict_reports,
                )
            self.assertEqual(strict_errors, 1)

    def test_plugin_path_parser_and_commands_persist_prefix(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            config = root / "config.json"
            prefix = root / "plugins"
            prefix.mkdir()
            with mock.patch.dict(os.environ, {"CASCADE_CONFIG_FILE": str(config)}, clear=False):
                args = cli.build_parser().parse_args(["plugin", "path", "add", str(prefix), "--json"])
                with contextlib.redirect_stdout(io.StringIO()):
                    args.func(args)
                self.assertEqual(cli.configured_plugin_prefixes(), [str(prefix.resolve())])

                listed = cli.build_parser().parse_args(["plugin", "path", "list", "--json"])
                output = io.StringIO()
                with contextlib.redirect_stdout(output):
                    listed.func(listed)
                payload = json.loads(output.getvalue())
                self.assertEqual(payload["plugin_prefixes"][0]["path"], str(prefix.resolve()))

                removed = cli.build_parser().parse_args(["plugin", "path", "remove", str(prefix), "--json"])
                with contextlib.redirect_stdout(io.StringIO()):
                    removed.func(removed)
                self.assertEqual(cli.configured_plugin_prefixes(), [])

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

            operations = cli._publish_staged_plugin(str(stage), str(target), package)
            self.assertEqual((target_package / "module.py").read_text(encoding="utf-8"), "new")
            cli._restore_publish_operations(operations)
            self.assertEqual((target_package / "module.py").read_text(encoding="utf-8"), "old")

    def test_plugin_install_publishes_then_registers_prefix(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            source = root / "source-plugin"
            target = root / "target"
            stage = target / ".cascade-plugin-stage-test"
            config = root / "config.json"
            source.mkdir()
            target.mkdir()
            (source / "SConstruct").write_text("", encoding="utf-8")
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
                    mock.patch.object(cli.tempfile, "mkdtemp", return_value=str(stage)), \
                    mock.patch.object(cli.subprocess, "run", return_value=completed) as run, \
                    mock.patch.object(cli, "_verify_staged_plugin", return_value={"packages": []}):
                with contextlib.redirect_stdout(io.StringIO()):
                    cli.cmd_plugin_install(args)

            installed = target / "lib" / "cascade" / "pyplugin" / source.name / "module.py"
            self.assertEqual(installed.read_text(encoding="utf-8"), "installed")
            config_document = json.loads(config.read_text(encoding="utf-8"))
            self.assertEqual(
                config_document["plugin_prefixes"],
                [{"enabled": True, "path": str(target.resolve())}],
            )
            build_environment = run.call_args.kwargs["env"]
            self.assertEqual(build_environment["CASCADE_PYPLUGIN_DIR"], str(stage / "lib" / "cascade" / "pyplugin"))

    def test_plugin_install_restores_previous_package_if_config_update_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            source = root / "rollback-plugin"
            target = root / "target"
            stage = target / ".cascade-plugin-stage-test"
            source.mkdir()
            target.mkdir()
            (source / "SConstruct").write_text("", encoding="utf-8")
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
            with mock.patch.object(cli.tempfile, "mkdtemp", return_value=str(stage)), \
                    mock.patch.object(cli.subprocess, "run", return_value=completed), \
                    mock.patch.object(cli, "_verify_staged_plugin", return_value={"packages": []}), \
                    mock.patch.object(cli, "add_plugin_prefix", side_effect=OSError("config failed")):
                with self.assertRaises(OSError), contextlib.redirect_stdout(io.StringIO()):
                    cli.cmd_plugin_install(args)
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
            with mock.patch.object(cli, "_load_controller", return_value=controller):
                cli.cmd_dag_run(args)

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
                cli.cmd_dag_run(args)

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
                cli.cmd_history(args)
            payload = json.loads(output.getvalue())
            self.assertEqual([entry["run_id"] for entry in payload["runs"]], ["run-newer"])

    def test_diff_ignores_run_identity_and_reports_parameter_changes(self):
        before = self._module_manifest("run-before", 10)
        after = self._module_manifest("run-after", 20)
        changes = cli._provenance_changes(
            cli._stable_provenance(before),
            cli._stable_provenance(after),
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
            with mock.patch.object(cli, "_load_controller", return_value=controller):
                with contextlib.redirect_stdout(io.StringIO()):
                    cli.cmd_replay(args)
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
                cli.cmd_replay(args)


if __name__ == "__main__":
    unittest.main()
