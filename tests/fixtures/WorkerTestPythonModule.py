from pathlib import Path

from cascade.pymodule import base_module


class WorkerTestPythonModule(base_module):
    METADATA = {
        "name": "WorkerTestPythonModule",
        "version": "1",
        "summary": "Python exec-worker integration fixture",
        "tags": ["test"],
    }

    def __init__(self):
        super().__init__()
        self.basename = "WorkerTestPythonModule"
        self.code_version_hash = "worker-python-test-v1"
        self.params["force_run"] = True

    def init(self):
        pass

    def execute(self):
        Path(self.stage_output("python-worker-result.txt")).write_text(
            "python-exec-worker", encoding="utf-8"
        )

    def finalize(self):
        pass
