import argparse
from pathlib import Path

from cascade import py_amcm


def main():
    parser = argparse.ArgumentParser(description="Run the mixed C++/Python Cascade example")
    parser.add_argument("--output", default="example-output")
    parser.add_argument("--isolated", action="store_true", help="Run every module in a subprocess")
    args = parser.parse_args()

    output_dir = Path(args.output).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    cache_dir = output_dir / ".cache"
    controller = py_amcm()

    modules = {
        "text_cpp": controller.register_module("TextProducerModule", "text_cpp"),
        "text_python": controller.register_module("TextTransformModule", "text_python"),
        "root_cpp": controller.register_module("RootEventModule", "root_cpp"),
        "root_python": controller.register_module("RootSummaryModule", "root_python"),
    }
    for module in modules.values():
        module.set_output_directory(str(output_dir))
        module.set_cache_directory(str(cache_dir))
        module.set_param("force_run", True)

    controller.add_module_to_dag("text_cpp", isolated=args.isolated)
    controller.add_module_to_dag("text_python", ["text_cpp"], isolated=args.isolated)
    controller.add_module_to_dag("root_cpp", isolated=args.isolated)
    controller.add_module_to_dag("root_python", ["root_cpp"], isolated=args.isolated)
    dag = controller.get_dag()
    result = controller.run_dag(fail_fast=False)
    dag.dump_dot(str(output_dir / "mixed_pipeline.dot"))
    if result.failed():
        failures = [
            f"{node.name}: {node.message}"
            for node in result.nodes
            if not node.succeeded()
        ]
        raise RuntimeError("DAG failed: " + "; ".join(failures))

    print(f"Outputs committed under {output_dir}")


if __name__ == "__main__":
    main()
