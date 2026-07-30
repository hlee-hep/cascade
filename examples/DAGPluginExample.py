from pathlib import Path

from cascade import py_amcm


def main():
    ctrl = py_amcm()

    required = {"TextProducerModule", "TextTransformModule"}
    missing = required - set(ctrl.get_list_available_modules())
    if missing:
        raise SystemExit(
            f"Missing modules {sorted(missing)}. Install examples/plugins/mixed_pipeline first."
        )

    output_dir = Path("dag-plugin-output").resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    producer = ctrl.register_module("TextProducerModule", "producer")
    transform = ctrl.register_module("TextTransformModule", "transform")
    for module in (producer, transform):
        module.set_output_directory(str(output_dir))
        module.set_cache_directory(str(output_dir / ".cache"))
        module.set_param("force_run", True)

    ctrl.add_module_to_dag("producer")
    ctrl.add_module_to_dag("transform", ["producer"])
    dag = ctrl.get_dag()
    result = ctrl.run_dag()
    dag.dump_dot(str(output_dir / "dag_plugin_example.dot"))
    if result.failed():
        failures = [
            f"{node.name}: {node.message}"
            for node in result.nodes
            if not node.succeeded()
        ]
        raise RuntimeError("DAG failed: " + "; ".join(failures))


if __name__ == "__main__":
    main()
