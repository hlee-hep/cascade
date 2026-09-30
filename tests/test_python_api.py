import unittest
from unittest import mock
from pathlib import Path
import tempfile

import cascade


class PythonApiTests(unittest.TestCase):
    def test_recording_failure_preserves_execution_result_and_resets_state(self):
        controller = cascade.Controller(discover_plugins=False)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output = root / "result.txt"
            controller.get_dag().add_node("write", [], lambda: output.write_text("committed"))
            blocker = root / "not-a-directory"
            blocker.write_text("existing")
            invalid = str(blocker / "workflow.json")
            with mock.patch("cascade.py_amcm.log") as log:
                result = controller.run_dag(provenance_path=invalid)
            self.assertTrue(result.succeeded())
            self.assertEqual(output.read_text(), "committed")
            self.assertEqual(blocker.read_text(), "existing")
            self.assertEqual(controller.last_workflow_provenance_path, "")
            self.assertTrue(controller.last_workflow_provenance_error)
            self.assertIn("DAG execution completed", log.call_args.args[2])
            with self.assertRaises(RuntimeError):
                controller.save_provenance(invalid)
            controller.get_dag().reset()
            with self.assertRaises(cascade.WorkflowProvenanceError) as failure:
                controller.run_dag(provenance_path=invalid, require_provenance=True)
            self.assertTrue(failure.exception.result.succeeded())
            self.assertIs(failure.exception.__cause__, failure.exception.provenance_error)
            controller.get_dag().reset()
            valid = str(root / "workflow.json")
            self.assertTrue(controller.run_dag(provenance_path=valid).succeeded())
            self.assertEqual(controller.last_workflow_provenance_path, valid)
            self.assertEqual(controller.last_workflow_provenance_error, "")

    def test_recording_failure_does_not_hide_failed_analysis(self):
        controller = cascade.Controller(discover_plugins=False)
        def fail():
            raise RuntimeError("analysis failure")
        controller.get_dag().add_node("fail", [], fail)
        with tempfile.TemporaryDirectory() as directory:
            blocker = Path(directory) / "file"
            blocker.touch()
            with mock.patch("cascade.py_amcm.log") as log:
                result = controller.run_dag(provenance_path=str(blocker / "workflow.json"))
            self.assertTrue(result.failed())
            self.assertIn("analysis failure", result.nodes[0].message)
            self.assertIn("DAG execution failed", log.call_args.args[2])

    def test_module_handles_preserve_parameter_types_and_return_copies(self):
        from cascade.py_amcm import _ModuleHandle
        from cascade.pymodule.base_module import base_module

        values = {"integer": 2**55 + 1, "flag": True, "real": 1.25,
                  "text": "sample", "empty": [], "mixed": [True, 2, 3.5, "x"],
                  "integers": [1, 2], "reals": [1.5, 2.5], "strings": ["a", "b"]}
        for language, module in (("cpp", cascade.IAnalysisModule()), ("python", base_module())):
            with self.subTest(language=language):
                for name, value in values.items():
                    module.register_param(name, value)
                handle = _ModuleHandle(None, module, language)
                actual = handle.get_parameters()
                for name, value in values.items():
                    self.assertEqual(actual[name], value)
                    self.assertIs(type(actual[name]), type(value))
                self.assertIs(type(actual["mixed"][0]), bool)
                actual["integers"].append(99)
                self.assertEqual(handle.get_parameters()["integers"], [1, 2])

    def test_controller_captures_independent_runtime_options(self):
        single_worker = cascade.RuntimeOptions()
        single_worker.dag_workers = 1
        four_workers = cascade.RuntimeOptions()
        four_workers.dag_workers = 4

        first = cascade.Controller(
            discover_plugins=False, runtime_options=single_worker
        )
        second = cascade.Controller(
            discover_plugins=False, runtime_options=four_workers
        )

        self.assertEqual(first.ctrl.get_runtime_options().dag_workers, 1)
        self.assertEqual(second.ctrl.get_runtime_options().dag_workers, 4)


if __name__ == "__main__":
    unittest.main()
