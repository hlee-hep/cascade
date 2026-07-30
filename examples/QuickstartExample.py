from pathlib import Path

from cascade import py_amcm, log_level, set_log_level


def main():
    ctrl = py_amcm()
    set_log_level(log_level.INFO)

    available = ctrl.get_list_available_modules()
    print("Available plugin modules:", available)

    if "TextProducerModule" not in available:
        raise SystemExit(
            "TextProducerModule is unavailable. Install examples/plugins/mixed_pipeline first."
        )

    output_dir = Path("quickstart-output").resolve()
    module = ctrl.register_module("TextProducerModule", "producer")
    module.set_output_directory(str(output_dir))
    module.set_cache_directory(str(output_dir / ".cache"))
    module.set_param("message", "hello from the quickstart")
    module.set_param("force_run", True)

    result = ctrl.run_module("producer")
    if result.failed():
        raise RuntimeError(f"{result.phase}: {result.message}")

    print("Registered modules:", ctrl.get_list_registered_modules())
    print("producer status:", ctrl.get_status("producer"))
    print("Progress:", ctrl.get_all_progress())
    print("Output:", output_dir / "message.json")


if __name__ == "__main__":
    main()
