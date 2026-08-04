from cascade.pymodule import base_module
from cascade._cascade import AMCM, IAnalysisModule, PluginPaths, PluginTrustPolicy, PluginVerifier
from cascade import init_interrupt, is_interrupted, log, log_level
import cascade
import importlib
import json
import os
import sys
import types

_PYPLUGIN_CACHE = None
_PYPLUGIN_CACHE_KEY = None


def _status_text(value):
    text = getattr(value, "value", None)
    if text is not None:
        return str(text)
    text = getattr(value, "name", str(value).rsplit(".", 1)[-1])
    if text == "None_":
        return "None"
    return text.title() if text.isupper() else text


def _python_plugin_roots():
    return list(PluginPaths.roots("python"))


def _default_trust_store(plugin_root):
    return PluginPaths.trust_store_for_root(plugin_root)


def _import_python_plugin(info):
    source_bytes = info.get("source_bytes", b"")
    module_name = info["module"]
    if module_name in sys.modules:
        return sys.modules[module_name]

    namespace_name = "cascade.pyplugin"
    if namespace_name not in sys.modules:
        namespace = types.ModuleType(namespace_name)
        namespace.__path__ = [os.path.dirname(info["package_dir"])]
        namespace.__package__ = namespace_name
        sys.modules[namespace_name] = namespace

    package_name = module_name.rsplit(".", 1)[0]
    if package_name not in sys.modules:
        package = types.ModuleType(package_name)
        package.__path__ = [info["package_dir"]]
        package.__package__ = package_name
        sys.modules[package_name] = package

    module = types.ModuleType(module_name)
    module.__file__ = info["path"]
    module.__package__ = package_name
    module.__loader__ = None
    sys.modules[module_name] = module
    try:
        code = compile(source_bytes, info["path"], "exec")
        exec(code, module.__dict__)
    except Exception:
        sys.modules.pop(module_name, None)
        raise
    return module


def _plugin_cache_key(plugin_roots, require_signed=False):
    trust_stores = PluginPaths.unique([_default_trust_store(root) for root in plugin_roots])
    return (
        tuple(plugin_roots),
        tuple(trust_stores),
        bool(require_signed),
        PluginVerifier.index_fingerprint(plugin_roots, trust_stores),
    )


def _load_python_plugin_index(require_signed=False):
    global _PYPLUGIN_CACHE, _PYPLUGIN_CACHE_KEY
    plugin_roots = _python_plugin_roots()
    cache_key = _plugin_cache_key(plugin_roots, require_signed)
    if _PYPLUGIN_CACHE is not None and _PYPLUGIN_CACHE_KEY == cache_key:
        return _PYPLUGIN_CACHE

    index = {}
    policy = PluginTrustPolicy.RequireSigned if require_signed else PluginTrustPolicy.Verified
    discovery = PluginVerifier.discover(plugin_roots, policy, "python")
    for error in discovery.errors:
        log(log_level.WARN, "PLUGIN", error)
    for package in discovery.packages:
        root = os.path.dirname(package.manifest_path)
        for artifact in package.artifacts:
            safe_package = "".join(
                character if character.isalnum() or character == "_" else "_"
                for character in package.package
            )
            package_token = package.manifest_sha256[:12]
            source_token = artifact.sha256[:12]
            modname = (
                f"cascade.pyplugin.{safe_package}_{package_token}."
                f"{os.path.splitext(os.path.basename(artifact.path))[0]}_{source_token}"
            )
            for class_name in artifact.classes:
                if class_name in index:
                    previous = index[class_name]
                    raise RuntimeError(
                        "Duplicate python plugin module name "
                        f"{class_name}: {previous['path']} and {artifact.path}"
                    )
                index[class_name] = {
                    "module": modname,
                    "class": class_name,
                    "path": artifact.path,
                    "package_dir": root,
                    "manifest": package.manifest_path,
                    "trusted_key": package.trusted_key_path or None,
                    "sha256": artifact.sha256,
                    "source_bytes": artifact.source,
                    "origin": {
                        "package": package.package,
                        "trust": _status_text(package.trust),
                        "manifest_path": package.manifest_path,
                        "manifest_sha256": package.manifest_sha256,
                        "artifact_sha256": artifact.sha256,
                        "signer_fingerprint": package.signer_fingerprint or None,
                    },
                }

    _PYPLUGIN_CACHE = index
    _PYPLUGIN_CACHE_KEY = cache_key
    return index


class _ModuleHandle:
    def __init__(self, ctrl, module, language):
        self._ctrl = ctrl
        self._module = module
        self.language = language

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
        if self.language == "python":
            return self._module.get_parameters()
        data = json.loads(self._module.dump_params_to_json(4))
        return {key: value["value"] for key, value in data.items()}

    def set_param(self, key, value):
        return self._module.set_param(key, value)

    def run(self):
        target = self._module if self.language == "python" else self.name()
        return self._ctrl.run_module(target)

    def run_isolated(self):
        target = self._module if self.language == "python" else self.name()
        return self._ctrl.run_module_isolated(target)


class py_amcm:
    def __init__(self, require_signed=False):
        self.require_signed = bool(require_signed)
        policy = PluginTrustPolicy.RequireSigned if self.require_signed else PluginTrustPolicy.Verified
        self.ctrl = AMCM(policy)
        self._module_name_counters = {}
        self.last_workflow_provenance_path = ""
        init_interrupt()

    def _register_python_plugin(self, class_name, instance_name):
        index = _load_python_plugin_index(self.require_signed)
        info = index.get(class_name)
        if not info:
            raise RuntimeError(f"Module not found: {class_name}")
        mod = _import_python_plugin(info)
        cls = getattr(mod, info["class"])
        module_obj = cls()
        if not isinstance(module_obj, base_module):
            raise TypeError("Module must inherit from cascade.pymodule.base_module")
        module_obj.set_name(instance_name)
        module_obj.set_plugin_origin(info["origin"])
        self.ctrl.register_module_handle(module_obj)
        handle = _ModuleHandle(self.ctrl, module_obj, "python")
        log(log_level.INFO, "CONTROL", f"Module {module_obj.get_basename()} is registered as {instance_name}")
        return handle

    def register_module(self, class_name, name=None):
        if name is None:
            count = self._module_name_counters.get(class_name, 0)
            while True:
                count += 1
                instance_name = f"{class_name}_{count}"
                if instance_name not in self.ctrl.get_list_registered_modules():
                    break
            self._module_name_counters[class_name] = count
        else:
            instance_name = name
        if instance_name in self.ctrl.get_list_registered_modules():
            raise RuntimeError(f"Module instance already registered: {instance_name}")
        cpp_modules = set(self.ctrl.get_list_available_modules())
        py_modules = set(_load_python_plugin_index(self.require_signed).keys())
        if class_name in cpp_modules and class_name in py_modules:
            raise RuntimeError(f"Duplicate module name across C++ and Python plugins: {class_name}")
        if class_name in cpp_modules:
            module = self.ctrl.register_module(class_name, instance_name)
            return _ModuleHandle(self.ctrl, module, "cpp")
        return self._register_python_plugin(class_name, instance_name)

    def register_python_module(self, name, module_obj):
        if name in self.ctrl.get_list_registered_modules():
            raise RuntimeError(f"Module instance already registered: {name}")
        if not isinstance(module_obj, base_module):
            raise TypeError("Module must inherit from cascade.pymodule.base_module")
        modname = module_obj.__class__.__module__
        if not modname.startswith("cascade.pyplugin."):
            raise RuntimeError(f"Python module {modname} is not a verified cascade.pyplugin module; refusing to register it.")
        info = _load_python_plugin_index(self.require_signed).get(module_obj.__class__.__name__)
        if not info or os.path.realpath(info["path"]) != os.path.realpath(sys.modules[modname].__file__):
            raise RuntimeError(f"Python module {module_obj.__class__.__name__} is not present in the verified plugin index.")
        module_obj.set_name(name)
        module_obj.set_plugin_origin(info["origin"])
        self.ctrl.register_module_handle(module_obj)
        return _ModuleHandle(self.ctrl, module_obj, "python")

    def _module_handle(self, name):
        module = self.ctrl.get_module(name)
        if isinstance(module, base_module):
            return _ModuleHandle(self.ctrl, module, "python")
        return _ModuleHandle(self.ctrl, module, "cpp")

    def run_module(self, name_or_mod, isolated=False):
        if isinstance(name_or_mod, str):
            handle = self._module_handle(name_or_mod)
        elif isinstance(name_or_mod, _ModuleHandle):
            handle = name_or_mod
        elif isinstance(name_or_mod, base_module):
            raise RuntimeError("Direct Python module execution is disabled; register a verified cascade.pyplugin module by name.")
        else:
            raise TypeError(f"Unsupported argument type: {type(name_or_mod)}")
        return handle.run_isolated() if isolated else handle.run()

    def run_module_isolated(self, name_or_mod):
        return self.run_module(name_or_mod, isolated=True)

    def get_list_available_modules(self):
        cpp_modules = set(self.ctrl.get_list_available_modules())
        py_modules = set(_load_python_plugin_index(self.require_signed).keys())
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
        for class_name, info in _load_python_plugin_index(self.require_signed).items():
            if class_name in cpp_names:
                raise RuntimeError(f"Duplicate module name across C++ and Python plugins: {class_name}")
            try:
                mod = _import_python_plugin(info)
                obj = getattr(mod, class_name)
            except Exception as error:
                log(
                    log_level.WARN,
                    "PLUGIN",
                    f"Cannot load metadata for {class_name}: {error}",
                )
                metadata.append({
                    "name": class_name,
                    "version": "",
                    "summary": "metadata unavailable",
                    "tags": [],
                    "language": "python",
                })
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
        return list(self.ctrl.get_list_registered_modules())

    def get_status(self, name):
        return self.ctrl.get_status(name)

    def get_all_progress(self):
        return self.ctrl.get_all_progress()

    def get_module(self, name):
        try:
            return self._module_handle(name)
        except RuntimeError:
            return None

    def get_python_module(self, name):
        module = self.get_module(name)
        return module._module if module is not None and module.language == "python" else None

    def get_dag(self):
        return self.ctrl.get_dag()

    def add_module_to_dag(self, name, dependencies=None, isolated=False):
        self.ctrl.add_module_to_dag(name, list(dependencies or []), bool(isolated))

    def link_dag_parameter(self, from_node, from_key, to_node, to_key):
        self.ctrl.link_dag_module_parameter(from_node, from_key, to_node, to_key)

    def run_dag(self, fail_fast=True, provenance_path=None):
        result = self.ctrl.run_dag(fail_fast)
        self.last_workflow_provenance_path = self.save_provenance(
            provenance_path, fail_fast=fail_fast
        )
        return result

    def save_run_log(self):
        return self.save_provenance()

    def run_group(self, group, fail_fast=True):
        if isinstance(group, (list, tuple)):
            names = [item if isinstance(item, str) else item.name() for item in group]
            return self.ctrl.run_group(names, bool(fail_fast))
        else:
            raise TypeError(f"Unsupported argument type: {type(group)}")

    def save_provenance(self, path=None, fail_fast=True, dag_result=None):
        saved = self.ctrl.save_provenance(
            os.path.abspath(os.path.expanduser(path)) if path else "",
            bool(fail_fast),
        )
        self.last_workflow_provenance_path = saved
        return saved

    def save_run_log_all(self, log_dir=None):
        if log_dir:
            workflow_id = cascade.ProvenanceRecorder.make_workflow_run_id()
            return self.save_provenance(
                os.path.join(log_dir, f"{workflow_id}.json")
            )
        return self.save_provenance()
