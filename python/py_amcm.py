from cascade.pymodule import base_module
from cascade._cascade import AMCM, IAnalysisModule
from cascade import init_interrupt, is_interrupted, log, log_level
import ast
import hashlib
import importlib
import importlib.util
import json
import os
import subprocess
import yaml
from datetime import datetime


_PYPLUGIN_CACHE = None


def _sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def _verify_manifest(manifest_path, public_key_path):
    sig_path = manifest_path + ".sig"
    if not os.path.exists(manifest_path) or not os.path.exists(sig_path):
        raise RuntimeError(f"Signed plugin manifest missing: {manifest_path}")
    cmd = [
        "openssl", "pkeyutl", "-verify", "-pubin",
        "-inkey", public_key_path, "-rawin",
        "-in", manifest_path, "-sigfile", sig_path,
    ]
    result = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if result.returncode != 0:
        raise RuntimeError(f"Plugin manifest signature invalid: {manifest_path}")


def _is_base_module(base):
    if isinstance(base, ast.Name):
        return base.id == "base_module"
    if isinstance(base, ast.Attribute):
        return base.attr == "base_module"
    return False


def _classes_from_file(path):
    try:
        with open(path, "r", encoding="utf-8") as f:
            tree = ast.parse(f.read(), filename=path)
    except Exception:
        return []
    classes = []
    for node in ast.walk(tree):
        if isinstance(node, ast.ClassDef):
            if any(_is_base_module(base) for base in node.bases):
                classes.append(node.name)
    return classes


def _load_python_plugin_index():
    global _PYPLUGIN_CACHE
    if _PYPLUGIN_CACHE is not None:
        return _PYPLUGIN_CACHE

    index = {}
    try:
        pkg = importlib.import_module("cascade.pyplugin")
    except Exception:
        _PYPLUGIN_CACHE = index
        return index

    for root in pkg.__path__:
        manifest_path = os.path.join(root, "plugin_manifest.json")
        public_key_path = os.path.join(root, "plugin_pubkey.pem")
        if not os.path.exists(public_key_path):
            log(log_level.WARN, "PLUGIN", f"Python plugin public key not found in {root}; skipping python plugins.")
            continue
        try:
            _verify_manifest(manifest_path, public_key_path)
            with open(manifest_path, "r", encoding="utf-8") as f:
                manifest = json.load(f)
        except Exception as e:
            log(log_level.WARN, "PLUGIN", str(e))
            continue

        for entry in manifest.get("modules", []):
            if entry.get("language") != "python":
                continue
            rel_path = entry.get("path", "")
            if not rel_path or os.path.isabs(rel_path):
                log(log_level.WARN, "PLUGIN", f"Ignoring invalid python plugin manifest path: {rel_path}")
                continue
            path = os.path.join(root, rel_path)
            if not os.path.exists(path):
                log(log_level.WARN, "PLUGIN", f"Manifest-listed python plugin file missing: {path}")
                continue
            if _sha256_file(path) != entry.get("sha256", ""):
                log(log_level.WARN, "PLUGIN", f"Python plugin hash mismatch: {path}")
                continue
            modname = "cascade.pyplugin." + os.path.splitext(os.path.basename(path))[0]
            for class_name in entry.get("classes") or _classes_from_file(path):
                if class_name in index:
                    previous = index[class_name]
                    raise RuntimeError(
                        "Duplicate python plugin module name "
                        f"{class_name}: {previous['path']} and {path}"
                    )
                index[class_name] = {
                    "module": modname,
                    "class": class_name,
                    "path": path,
                    "manifest": manifest_path,
                }

    _PYPLUGIN_CACHE = index
    return index


class _CppModuleHandle:
    language = "cpp"

    def __init__(self, ctrl, module):
        self._ctrl = ctrl
        self._module = module

    def __getattr__(self, name):
        return getattr(self._module, name)

    def name(self):
        return self._module.name()

    def get_basename(self):
        return self._module.get_basename()

    def get_status(self):
        return self._module.get_status()

    def get_code_hash(self):
        return self._module.get_code_hash()

    def get_parameters(self):
        raw = self._module.dump_params_to_json(4)
        data = json.loads(raw)
        return {k: v["value"] for k, v in data.items()}

    def set_param(self, key, value):
        return self._module.set_param(key, value)

    def run(self):
        return self._ctrl.run_module(self.name())


class _PythonModuleHandle:
    language = "python"

    def __init__(self, module):
        self._module = module

    def __getattr__(self, name):
        return getattr(self._module, name)

    def name(self):
        return self._module.name()

    def get_basename(self):
        return self._module.get_basename()

    def get_status(self):
        return self._module.get_status()

    def get_code_hash(self):
        return self._module.get_code_hash()

    def get_parameters(self):
        return self._module.get_parameters()

    def set_param(self, key, value):
        return self._module.set_param(key, value)

    def run(self):
        return self._module.run()


class py_amcm:
    def __init__(self):
        self.ctrl = AMCM()
        self.modules = {}
        self.executed_modules = []
        init_interrupt()

    def _register_python_plugin(self, class_name, instance_name):
        index = _load_python_plugin_index()
        info = index.get(class_name)
        if not info:
            raise RuntimeError(f"Module not found: {class_name}")
        mod = importlib.import_module(info["module"])
        cls = getattr(mod, info["class"])
        module_obj = cls()
        if not isinstance(module_obj, base_module):
            raise TypeError("Module must inherit from PythonModuleBase")
        module_obj.set_name(instance_name)
        handle = _PythonModuleHandle(module_obj)
        self.modules[instance_name] = handle
        log(log_level.INFO, "CONTROL", f"Module {module_obj.get_basename()} is registered as {instance_name}")
        return handle

    def register_module(self, class_name, name=None):
        instance_name = name or f"{class_name}_{sum(1 for h in self.modules.values() if h.get_basename() == class_name) + 1}"
        cpp_modules = set(self.ctrl.get_list_available_modules())
        py_modules = set(_load_python_plugin_index().keys())
        if class_name in cpp_modules and class_name in py_modules:
            raise RuntimeError(f"Duplicate module name across C++ and Python plugins: {class_name}")
        if class_name in cpp_modules:
            module = self.ctrl.register_module(class_name, instance_name)
            handle = _CppModuleHandle(self.ctrl, module)
            self.modules[instance_name] = handle
            return handle
        return self._register_python_plugin(class_name, instance_name)

    def register_python_module(self, name, module_obj):
        modname = module_obj.__class__.__module__
        if not modname.startswith("cascade.pyplugin."):
            raise RuntimeError(f"Python module {modname} is not a signed cascade.pyplugin module; refusing to register it.")
        return self.register_module(module_obj.__class__.__name__, name)

    def run_module(self, name_or_mod):
        if is_interrupted():
            log(log_level.WARN, "CONTROL", "Global SIGINT detected. Skipping the module...")
            return
        if isinstance(name_or_mod, str):
            handle = self.modules.get(name_or_mod)
            if handle is None:
                raise RuntimeError(f"Module not registered: {name_or_mod}")
        elif isinstance(name_or_mod, (_CppModuleHandle, _PythonModuleHandle)):
            handle = name_or_mod
        elif isinstance(name_or_mod, IAnalysisModule):
            handle = _CppModuleHandle(self.ctrl, name_or_mod)
        elif isinstance(name_or_mod, base_module):
            raise RuntimeError("Direct Python module execution is disabled; register a signed cascade.pyplugin module by name.")
        else:
            raise TypeError(f"Unsupported argument type: {type(name_or_mod)}")
        handle.run()
        self.executed_modules.append(handle)

    def get_list_available_modules(self):
        cpp_modules = set(self.ctrl.get_list_available_modules())
        py_modules = set(_load_python_plugin_index().keys())
        duplicates = sorted(cpp_modules & py_modules)
        if duplicates:
            raise RuntimeError(f"Duplicate module names across C++ and Python plugins: {duplicates}")
        return sorted(cpp_modules | py_modules)

    def get_list_available_module_metadata(self, include_python=True, instantiate_python=False):
        metadata = []
        for item in self.ctrl.get_list_available_module_metadata():
            metadata.append({
                "name": item.name,
                "version": item.version,
                "summary": item.summary,
                "tags": list(item.tags),
                "language": "cpp",
            })
        if not include_python:
            return metadata

        cpp_names = {entry["name"] for entry in metadata}
        for class_name, info in _load_python_plugin_index().items():
            if class_name in cpp_names:
                raise RuntimeError(f"Duplicate module name across C++ and Python plugins: {class_name}")
            try:
                mod = importlib.import_module(info["module"])
                obj = getattr(mod, class_name)
            except Exception:
                continue
            plugin_info = getattr(obj, "METADATA", None)
            if isinstance(plugin_info, dict):
                metadata.append({
                    "name": plugin_info.get("name", class_name),
                    "version": plugin_info.get("version", ""),
                    "summary": plugin_info.get("summary", ""),
                    "tags": list(plugin_info.get("tags", [])),
                    "language": "python",
                })
            elif instantiate_python:
                try:
                    plugin_info = obj().get_metadata()
                except Exception:
                    plugin_info = None
                if isinstance(plugin_info, dict):
                    metadata.append({
                        "name": plugin_info.get("name", class_name),
                        "version": plugin_info.get("version", ""),
                        "summary": plugin_info.get("summary", ""),
                        "tags": list(plugin_info.get("tags", [])),
                        "language": "python",
                    })
            else:
                import cascade
                metadata.append({
                    "name": class_name,
                    "version": getattr(obj, "VERSION", getattr(cascade, "__version__", "")),
                    "summary": getattr(obj, "SUMMARY", ""),
                    "tags": list(getattr(obj, "TAGS", [])),
                    "language": "python",
                })
        return metadata

    def get_list_registered_modules(self):
        return list(self.modules.keys())

    def get_status(self, name):
        handle = self.modules.get(name)
        if handle is None:
            raise RuntimeError(f"Module not registered: {name}")
        return handle.get_status()

    def get_all_progress(self):
        progress = self.ctrl.get_all_progress()
        for name, handle in self.modules.items():
            if handle.language == "python":
                progress.setdefault(name, {})
        return progress

    def get_module(self, name):
        return self.modules.get(name, None)

    def get_python_module(self, name):
        handle = self.modules.get(name)
        if isinstance(handle, _PythonModuleHandle):
            return handle._module
        return None

    def get_dag(self):
        return self.ctrl.get_dag()

    def run_dag(self):
        self.ctrl.run_dag()

    def save_run_log(self):
        self.ctrl.save_run_log()

    def run_group(self, group):
        if isinstance(group, (list, tuple)):
            for mod in group:
                self.run_module(mod)
        else:
            raise TypeError(f"Unsupported argument type: {type(group)}")

    def save_run_log_all(self, log_dir=None):
        now = datetime.now()
        timestamp = now.strftime("%Y%m%d_%H%M%S")
        log_data = {"modules": []}

        filename_suffix = []
        for i, handle in enumerate(self.executed_modules):
            entry = {
                "name": handle.name(),
                "module": handle.get_basename(),
                "codehash": handle.get_code_hash(),
                "status": handle.get_status(),
                "params": handle.get_parameters(),
            }
            log_data["modules"].append(entry)
            if i < 5:
                filename_suffix.append(handle.name())

        suffix = "_".join(filename_suffix)
        filename = f"control_log_{timestamp}_{suffix}.yaml"
        default_log_dir = os.path.join(os.path.expanduser("~"), ".cache", "cascade", "run_logs")
        log_dir = log_dir or os.getenv("CASCADE_RUN_LOG_DIR", default_log_dir)
        os.makedirs(log_dir, exist_ok=True)

        with open(os.path.join(log_dir, filename), "w") as f:
            yaml.dump(log_data, f, sort_keys=False)

        log(log_level.INFO, "CONTROL", f"Run log '{filename}' is saved in {log_dir}.")
