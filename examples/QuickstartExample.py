from cascade import py_amcm, log_level, set_log_level


def main():
    ctrl = py_amcm()

    set_log_level(log_level.INFO)

    available = ctrl.get_list_available_modules()
    print("Available plugin modules:", available)

    if "ExampleModule" in available:
        cpp_mod = ctrl.register_module("ExampleModule", "cpp_stage")
        cpp_mod.set_param("force_run", True)
        ctrl.run_module("cpp_stage")
        print("cpp_stage status:", ctrl.get_status("cpp_stage"))
    else:
        print("ExampleModule is not available. Build, sign, and install /home/hlee/cascade_plugin to run the C++ plugin stage.")

    print("Registered modules:", ctrl.get_list_registered_modules())
    print("Progress:", ctrl.get_all_progress())


if __name__ == "__main__":
    main()
