from cascade import py_amcm
from cascade.pymodule import base_module


class py_stage(base_module):
    VERSION = "1.0.0"
    SUMMARY = "Example Python stage"
    TAGS = ["example", "python"]

    def __init__(self):
        super().__init__()
        self.basename = "py_stage"

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

    ctrl.register_module("ExampleModule", "cpp_stage")
    ctrl.register_python_module("py_stage", py_stage())

    dag = ctrl.get_dag()
    dag.add_node("cpp_stage", [], lambda: ctrl.run_module("cpp_stage"))
    dag.add_node("py_stage", ["cpp_stage"], lambda: ctrl.run_module("py_stage"))

    dag.dump_dot("dag_example.dot")
    ctrl.run_dag()


if __name__ == "__main__":
    main()
