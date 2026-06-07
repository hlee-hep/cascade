from cascade import py_amcm


def main():
    ctrl = py_amcm()

    if "ExampleModule" not in ctrl.get_list_available_modules():
        raise SystemExit("ExampleModule is not available. Build, sign, and install /home/hlee/cascade_plugin before running this DAG example.")

    ctrl.register_module("ExampleModule", "cpp_stage")

    dag = ctrl.get_dag()
    dag.add_node("cpp_stage", [], lambda: ctrl.run_module("cpp_stage"))

    dag.dump_dot("dag_example.dot")
    ctrl.run_dag()


if __name__ == "__main__":
    main()
