from cascade import py_amcm, log_level, set_log_level
from cascade.pymodule import base_module


class py_quickstart(base_module):
    VERSION = "1.0.0"
    SUMMARY = "Quickstart Python stage"
    TAGS = ["quickstart", "python"]

    def __init__(self):
        super().__init__()
        self.basename = "py_quickstart"

    def print_description(self):
        pass

    def init(self):
        pass

    def execute(self):
        pass

    def finalize(self):
        pass


def main():
    ctrl = py_amcm()

    set_log_level(log_level.INFO)

    available = ctrl.get_list_available_modules()
    print("Available C++ modules:", available)
    print("Module metadata:", ctrl.get_list_available_module_metadata())

    cpp_mod = ctrl.register_module("ExampleModule", "cpp_stage")
    cpp_mod.set_param("force_run", True)
    ctrl.register_python_module("py_stage", py_quickstart())
    ctrl.get_python_module("py_stage").set_param("force_run", True)

    ctrl.run_module("cpp_stage")
    print("Registered modules:", ctrl.get_list_registered_modules())
    print("cpp_stage status:", ctrl.get_status("cpp_stage"))

    print("Progress:", ctrl.get_all_progress())
    ctrl.save_run_log_all()


if __name__ == "__main__":
    main()
