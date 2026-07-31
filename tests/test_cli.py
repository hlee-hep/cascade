import argparse
import importlib.machinery
import importlib.util
import json
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


class _FakeResult:
    def __init__(self, nodes=None):
        self.nodes = nodes or []

    def succeeded(self):
        return True

    def failed(self):
        return False


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

    def get_dag(self):
        return self.dag


class CliTests(unittest.TestCase):
    def test_key_value_parsing(self):
        self.assertEqual(cli._parse_kv("enabled=true"), ("enabled", True))
        self.assertEqual(cli._parse_kv("count=4"), ("count", 4))
        self.assertEqual(cli._parse_kv('items=["a", 2]'), ("items", ["a", 2]))
        with self.assertRaises(argparse.ArgumentTypeError):
            cli._parse_kv("=missing")

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


if __name__ == "__main__":
    unittest.main()
