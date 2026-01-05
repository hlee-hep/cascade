from cascade.pymodule import base_module
from cascade._cascade import AMCM, IAnalysisModule
from cascade import init_interrupt, is_interrupted, log, log_level
import yaml, os
from datetime import datetime
import json
#wrapper class for python module

class py_amcm:
    def __init__(self):
        self.ctrl = AMCM()  # py_amcm instance
        self.python_modules = {}  # name -> PythonModuleBase instance
        self.executed_modules = []
        init_interrupt()

    def register_python_module(self, name, module_obj):
        if not isinstance(module_obj, base_module):
            raise TypeError("Module must inherit from PythonModuleBase")
        self.python_modules[name] = module_obj
        self.python_modules[name].set_name(name)
        log(log_level.INFO,"CONTROL",f"Module {module_obj.get_basename()} is registered as {name}")

    def run_module(self, name_or_mod):
        if is_interrupted():
            log(log_level.WARN,"CONTROL","Global SIGINT detected. Skipping the module...")
            return 
        if isinstance(name_or_mod,str):
            name = name_or_mod
            if name in self.python_modules:
                self.python_modules[name].run()
                self.executed_modules.append(self.python_modules[name])
            else:
                self.ctrl.run_module(name)
                self.executed_modules.append(self.ctrl.get_module(name))
        elif isinstance(name_or_mod, base_module):
            mod = name_or_mod
            if mod in self.python_modules.values():
                mod.run()
                self.executed_modules.append(mod)
            else:
                print("No such module exists.")
        elif isinstance(name_or_mod, IAnalysisModule):
            mod = name_or_mod
            self.ctrl.run_module(mod)
            self.executed_modules.append(mod)
        else:
            raise TypeError(f"Unsupported argument type: {type(name_or_mod)}")

    def get_list_available_modules(self):
        import ast
        import importlib
        import importlib.util
        import pkgutil
        modules = self.ctrl.get_list_available_modules()

        def is_base_module(base):
            if isinstance(base, ast.Name):
                return base.id in ("base_module", "py_base_module")
            if isinstance(base, ast.Attribute):
                return base.attr in ("base_module", "py_base_module")
            return False

        def list_python_modules(pkg_name):
            try:
                pkg = importlib.import_module(pkg_name)
            except Exception:
                return []
            return [modname for _, modname, _ in pkgutil.iter_modules(pkg.__path__, pkg_name + ".")]

        def classes_from_module(modname):
            try:
                spec = importlib.util.find_spec(modname)
            except Exception:
                return []
            if spec is None or not spec.origin or not spec.origin.endswith(".py"):
                return []
            try:
                with open(spec.origin, "r", encoding="utf-8") as f:
                    tree = ast.parse(f.read(), filename=spec.origin)
            except Exception:
                return []
            classes = []
            for node in ast.walk(tree):
                if isinstance(node, ast.ClassDef):
                    for base in node.bases:
                        if is_base_module(base):
                            classes.append(node.name)
                            break
            return classes

        for modname in list_python_modules("cascade.pymodule"):
            modules.extend(classes_from_module(modname))
        for modname in list_python_modules("cascade.pyplugin"):
            modules.extend(classes_from_module(modname))
        return modules

    def get_list_registered_modules(self):
        modules = self.ctrl.get_list_registered_modules()
        modules.extend(self.python_modules.keys())
        return modules

    def get_python_module(self, name):
        return self.python_modules.get(name, None)

    def get_dag(self):
        return self.ctrl.get_dag()

    def run_dag(self):
        self.ctrl.run_dag()

    def save_run_log(self):
        self.ctrl.save_run_log()

    def register_module(self, class_name, name=None):
        if name is None:
            return self.ctrl.register_module(class_name)
        else:
            return self.ctrl.register_module(class_name, name)

    def run_group(self, group):
        if isinstance(group, (list, tuple)):
            for mod in group:
                self.run_module(mod)
        else:
            raise TypeError(f"Unsupported argument type: {type(group)}")

    def save_run_log_all(self, log_dir=None):
        now = datetime.now()
        timestamp = now.strftime("%Y%m%d_%H%M%S")
        log_data = {
        "modules": []
        }

        filename_suffix = []
        for i, mod in enumerate(self.executed_modules):
            if isinstance(mod,IAnalysisModule):
                raw = json.loads(mod.get_params_to_json())
                params = {k: v["value"] for k, v in raw.items()}
            elif isinstance(mod, base_module):
                params = mod.get_parameters()
            else:
                params = None
            entry = {
            "name": mod.name(),
            "module": mod.get_basename(),
            "codehash": mod.get_code_hash(),
            "status": mod.get_status(),
            "params": params
            }
            log_data["modules"].append(entry)
            if i < 5:
                filename_suffix.append(mod.name())

        suffix = "_".join(filename_suffix)
        filename = f"control_log_{timestamp}_{suffix}.yaml"
        default_log_dir = os.path.join(os.path.expanduser("~"), ".cache", "cascade", "run_logs")
        log_dir = log_dir or os.getenv("CASCADE_RUN_LOG_DIR", default_log_dir)
        os.makedirs(log_dir, exist_ok=True)

        with open(os.path.join(log_dir, filename), "w") as f:
            yaml.dump(log_data, f, sort_keys=False)

        log(log_level.INFO,"CONTROL",f"Run log '{filename}' is saved in {log_dir}.")
