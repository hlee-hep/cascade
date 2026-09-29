import unittest

import cascade


class PythonApiTests(unittest.TestCase):
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
