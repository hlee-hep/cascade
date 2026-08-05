import pathlib
import tempfile
import unittest

from cascade.py_amcm import py_amcm


class PythonWorkerIntegrationTests(unittest.TestCase):
    def test_verified_python_plugin_runs_in_exec_worker(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            controller = py_amcm()
            module = controller.register_module(
                "WorkerTestPythonModule", "python-worker-instance"
            )
            module.set_output_directory(root / "output")
            module.set_cache_directory(root / "cache")
            result = controller.run_module_isolated(module)
            self.assertEqual(result.status.value, "Done")
            self.assertEqual(result.cache_decision, "bypassed")
            refreshed = controller.refresh_plugins()
            self.assertEqual(refreshed["added_cpp"], [])
            self.assertEqual(refreshed["added_python"], [])
            self.assertEqual(
                (root / "output" / "python-worker-result.txt").read_text(
                    encoding="utf-8"
                ),
                "python-exec-worker",
            )


if __name__ == "__main__":
    unittest.main()
