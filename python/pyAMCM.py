from cascade.pymodule import pyBaseModule
from cascade import AMCM
from cascade._cascade import IAnalysisModule 
#wrapper class for python module

class pyAMCM:
    def __init__(self):
        self.ctrl = AMCM()  # pyAMCM instance
        self.python_modules = {}  # name -> PythonModuleBase instance

    def register_python_module(self, name, module_obj):
        if not isinstance(module_obj, pyBaseModule):
            raise TypeError("Module must inherit from PythonModuleBase")
        self.python_modules[name] = module_obj
        self.python_modules[name].set_name(name)

    def run_module(self, name_or_mod):
        if isinstance(name_or_mod,str):
            name = name_or_mod
            if name in self.python_modules:
                self.python_modules[name].run()
            else:
                self.ctrl.run_module(name)
        elif isinstance(name_or_mod,pyBaseModule):
            mod = name_or_mod
            if mod in self.python_modules.values():
                mod.init()
                mod.execute()
                mod.finalize()
            else:
                print("No such module exists.")
        elif isinstance(name_or_mod, IAnalysisModule):
            mod = name_or_mod
            self.ctrl.run_module(mod)
        else:
            raise TypeError(f"Unsupported argument type: {type(name_or_mod)}")

    def get_list_available_modules(self):
        import importlib
        import pkgutil
        import inspect
        modules = self.ctrl.get_list_available_modules()
        pkg = importlib.import_module("cascade.pymodule")
        for _, modname, _ in pkgutil.iter_modules(pkg.__path__, "cascade.pymodule" + "."):
            try:
                mod = importlib.import_module(modname)
            except Exception:
                continue
            for name, obj in inspect.getmembers(mod, inspect.isclass):
                if issubclass(obj, pyBaseModule) and obj is not pyBaseModule:
                    modules.append(name)
        return modules

    def get_list_registered_modules(self):
        modules = self.ctrl.get_list_registered_modules()
        for k, _ in self.python_modules:
            modules.append(k)
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
