import unittest

import cascade


class PythonApiTests(unittest.TestCase):
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
