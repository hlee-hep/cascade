import argparse
from pathlib import Path

from cascade import py_amcm


def main():
    parser = argparse.ArgumentParser(description="Run the self-contained Cascade toy dimuon analysis")
    parser.add_argument("--output", default="example-output", help="Directory receiving committed products")
    parser.add_argument("--events", type=int, default=30000, help="Number of generated dimuon candidates")
    parser.add_argument("--seed", type=int, default=42, help="Deterministic generator seed")
    parser.add_argument("--float-width", action="store_true", help="Float the natural width in the mass fit")
    parser.add_argument("--isolated", action="store_true", help="Run every module in an isolated worker")
    parser.add_argument("--force", action="store_true", help="Bypass snapshot cache checks")
    args = parser.parse_args()

    output_directory = Path(args.output).resolve()
    output_directory.mkdir(parents=True, exist_ok=True)
    cache_directory = output_directory / ".cache"
    controller = py_amcm()

    modules = {
        "generate": controller.register_module("ToyDimuonSourceModule", "generate"),
        "spectrum": controller.register_module("DimuonSpectrumModule", "spectrum"),
        "fit": controller.register_module("ResonanceFitModule", "fit"),
        "report": controller.register_module("MassReportModule", "report"),
    }
    for module in modules.values():
        module.set_output_directory(str(output_directory))
        module.set_cache_directory(str(cache_directory))
        module.set_param("force_run", args.force)

    modules["generate"].set_param("events", args.events)
    modules["generate"].set_param("random_seed", args.seed)
    modules["fit"].set_param("float_natural_width", args.float_width)

    controller.add_module_to_dag("generate", isolated=args.isolated)
    controller.add_module_to_dag("spectrum", ["generate"], isolated=args.isolated)
    controller.add_module_to_dag("fit", ["spectrum"], isolated=args.isolated)
    controller.add_module_to_dag("report", ["fit"], isolated=args.isolated)

    provenance_path = output_directory / "toy-dimuon-provenance.json"
    result = controller.run_dag(fail_fast=True, provenance_path=str(provenance_path))
    controller.get_dag().dump_dot(str(output_directory / "toy-dimuon-analysis.dot"))
    for node in result.nodes:
        detail = f" — {node.message}" if node.message else ""
        print(f"{node.name}: {modules[node.name].get_status()}{detail}")
    if result.failed():
        failures = [f"{node.name}: {node.message}" for node in result.nodes if not node.succeeded()]
        raise RuntimeError("DAG failed: " + "; ".join(failures))

    print(f"Outputs committed under {output_directory}")
    print(f"Report: {output_directory / 'dimuon_report.md'}")
    print(f"Workflow provenance: {controller.last_workflow_provenance_path}")


if __name__ == "__main__":
    main()
