from cascade.pymodule import base_module
from cascade._cascade import AMCM, IAnalysisModule
from cascade import init_interrupt, is_interrupted, log, log_level
import cascade
import ast
import hashlib
import importlib
import importlib.util
import json
import os
import subprocess
import yaml
import copy
import sys
import types
import re
import signal
import time
from datetime import datetime, timezone


_PYPLUGIN_CACHE = None
_PYPLUGIN_CACHE_KEY = None


def _utc_now():
    return datetime.now(timezone.utc).isoformat(timespec="microseconds").replace("+00:00", "Z")


def _atomic_write_json(path, document):
    path = os.path.abspath(os.path.expanduser(path))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    temporary = f"{path}.tmp.{os.getpid()}"
    try:
        with open(temporary, "w", encoding="utf-8") as output:
            json.dump(document, output, indent=2, sort_keys=False)
            output.write("\n")
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.remove(temporary)
    return path


def _status_text(value):
    text = getattr(value, "value", None)
    if text is not None:
        return str(text)
    text = getattr(value, "name", str(value).rsplit(".", 1)[-1])
    if text == "None_":
        return "None"
    return text.title() if text.isupper() else text


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


def _default_python_plugin_root():
    configured = os.getenv("CASCADE_PYPLUGIN_DIR")
    if configured:
        return os.path.abspath(configured)
    import cascade
    return os.path.join(os.path.dirname(os.path.abspath(cascade.__file__)), "pyplugin")


def _default_trust_store(plugin_root):
    configured = os.getenv("CASCADE_PLUGIN_TRUST_STORE")
    if configured:
        return os.path.abspath(configured)
    prefix = os.path.abspath(plugin_root)
    for _ in range(3):
        prefix = os.path.dirname(prefix)
    return os.path.join(prefix, "share", "cascade", "trusted_keys")


def _trusted_key_paths(plugin_root):
    trust_store = _default_trust_store(plugin_root)
    if not os.path.isdir(trust_store):
        log(log_level.WARN, "PLUGIN", f"Plugin trust store not found: {trust_store}")
        return []
    return [
        os.path.join(trust_store, name)
        for name in sorted(os.listdir(trust_store))
        if name.endswith(".pem") and os.path.isfile(os.path.join(trust_store, name))
    ]


def _verify_with_trusted_key(manifest_path, keys):
    for key in keys:
        try:
            _verify_manifest(manifest_path, key)
            return key
        except RuntimeError:
            pass
    return None


def _contained_path(root, relative):
    if not relative or os.path.isabs(relative):
        return None
    root = os.path.realpath(root)
    candidate = os.path.realpath(os.path.join(root, relative))
    try:
        if os.path.commonpath([root, candidate]) != root:
            return None
    except ValueError:
        return None
    return candidate


def _import_python_plugin(info):
    expected_hash = info.get("sha256", "")
    if not expected_hash or _sha256_file(info["path"]) != expected_hash:
        raise RuntimeError(f"Python plugin changed after verification: {info['path']}")
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

    spec = importlib.util.spec_from_file_location(module_name, info["path"])
    if spec is None or spec.loader is None:
        raise ImportError(f"Cannot create import spec for Python plugin: {info['path']}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module
    try:
        spec.loader.exec_module(module)
    except Exception:
        sys.modules.pop(module_name, None)
        raise
    return module


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
    for node in tree.body:
        if isinstance(node, ast.ClassDef):
            if any(_is_base_module(base) for base in node.bases):
                classes.append(node.name)
    return classes


def _plugin_cache_key(plugin_root):
    trust_store = _default_trust_store(plugin_root)
    tracked = []
    for root, _, files in os.walk(plugin_root) if os.path.isdir(plugin_root) else []:
        for name in files:
            if name.endswith((".py", ".json", ".sig")):
                path = os.path.join(root, name)
                try:
                    stat = os.stat(path)
                except FileNotFoundError:
                    continue
                tracked.append((os.path.realpath(path), stat.st_mtime_ns, stat.st_size))
    if os.path.isdir(trust_store):
        for name in os.listdir(trust_store):
            if name.endswith(".pem"):
                path = os.path.join(trust_store, name)
                if os.path.isfile(path):
                    try:
                        stat = os.stat(path)
                    except FileNotFoundError:
                        continue
                    tracked.append((os.path.realpath(path), stat.st_mtime_ns, stat.st_size))
    return os.path.realpath(plugin_root), os.path.realpath(trust_store), tuple(sorted(tracked))


def _load_python_plugin_index():
    global _PYPLUGIN_CACHE, _PYPLUGIN_CACHE_KEY
    plugin_root = _default_python_plugin_root()
    cache_key = _plugin_cache_key(plugin_root)
    if _PYPLUGIN_CACHE is not None and _PYPLUGIN_CACHE_KEY == cache_key:
        return _PYPLUGIN_CACHE

    index = {}
    if not os.path.isdir(plugin_root):
        _PYPLUGIN_CACHE = index
        _PYPLUGIN_CACHE_KEY = cache_key
        return index
    trusted_keys = _trusted_key_paths(plugin_root)
    if not trusted_keys:
        _PYPLUGIN_CACHE = index
        _PYPLUGIN_CACHE_KEY = cache_key
        return index

    package_dirs = [
        os.path.join(plugin_root, name)
        for name in sorted(os.listdir(plugin_root))
        if os.path.isdir(os.path.join(plugin_root, name))
    ]
    for root in package_dirs:
        manifest_path = os.path.join(root, "plugin_manifest.json")
        try:
            trusted_key = _verify_with_trusted_key(manifest_path, trusted_keys)
            if trusted_key is None:
                raise RuntimeError(f"Plugin manifest is not signed by a trusted key: {manifest_path}")
            with open(manifest_path, "r", encoding="utf-8") as f:
                manifest = json.load(f)
            if manifest.get("schema") != 2:
                raise RuntimeError(f"Unsupported plugin manifest schema: {manifest_path}")
            if manifest.get("package") != os.path.basename(root):
                raise RuntimeError(f"Plugin package name does not match directory: {manifest_path}")
        except Exception as e:
            log(log_level.WARN, "PLUGIN", str(e))
            continue

        for entry in manifest.get("modules", []):
            if entry.get("language") != "python":
                continue
            rel_path = entry.get("path", "")
            path = _contained_path(root, rel_path)
            if path is None:
                log(log_level.WARN, "PLUGIN", f"Ignoring invalid python plugin manifest path: {rel_path}")
                continue
            if not os.path.exists(path):
                log(log_level.WARN, "PLUGIN", f"Manifest-listed python plugin file missing: {path}")
                continue
            if _sha256_file(path) != entry.get("sha256", ""):
                log(log_level.WARN, "PLUGIN", f"Python plugin hash mismatch: {path}")
                continue
            safe_package = "".join(character if character.isalnum() or character == "_" else "_" for character in os.path.basename(root))
            package_token = hashlib.sha256(os.path.realpath(root).encode("utf-8")).hexdigest()[:12]
            source_token = entry.get("sha256", "")[:12]
            modname = (
                f"cascade.pyplugin.{safe_package}_{package_token}."
                f"{os.path.splitext(os.path.basename(path))[0]}_{source_token}"
            )
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
                    "package_dir": root,
                    "manifest": manifest_path,
                    "trusted_key": trusted_key,
                    "sha256": entry.get("sha256", ""),
                }

    _PYPLUGIN_CACHE = index
    _PYPLUGIN_CACHE_KEY = cache_key
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

    def run_isolated(self):
        return self._ctrl.run_module_isolated(self.name())


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

    def run_isolated(self):
        from cascade.pymodule.base_module import ModulePhase as PythonModulePhase
        from cascade.pymodule.base_module import ModuleStatus as PythonModuleStatus
        from cascade.pymodule.base_module import RunResult as PythonRunResult

        module = self._module
        module.prepare_external_run()
        try:
            read_fd, write_fd = os.pipe()
        except Exception as error:
            result = PythonRunResult(
                PythonModuleStatus.FAILED,
                PythonModulePhase.EXECUTE,
                str(error),
                error,
            )
            return module.adopt_external_run_result(result)

        try:
            child = os.fork()
        except Exception as error:
            os.close(read_fd)
            os.close(write_fd)
            result = PythonRunResult(
                PythonModuleStatus.FAILED,
                PythonModulePhase.EXECUTE,
                str(error),
                error,
            )
            return module.adopt_external_run_result(result)

        if child == 0:
            os.close(read_fd)
            try:
                for fatal_signal in (signal.SIGSEGV, signal.SIGABRT, signal.SIGBUS, signal.SIGILL, signal.SIGFPE):
                    signal.signal(fatal_signal, signal.SIG_DFL)
                result = module.run_prepared_external()
                payload = json.dumps({
                    "status": result.status.value,
                    "phase": result.phase.value,
                    "message": result.message[:4096],
                }).encode("utf-8")
                while payload:
                    written = os.write(write_fd, payload)
                    payload = payload[written:]
                os.close(write_fd)
                os._exit(0)
            except BaseException:
                os.close(write_fd)
                os._exit(125)

        os.close(write_fd)
        cancellation_sent = False
        cancellation_polls = 0
        child_status = 0
        while True:
            waited, child_status = os.waitpid(child, os.WNOHANG)
            if waited == child:
                break
            if module.is_cancellation_requested():
                if not cancellation_sent:
                    try:
                        os.kill(child, signal.SIGTERM)
                    except ProcessLookupError:
                        pass
                    cancellation_sent = True
                else:
                    cancellation_polls += 1
                    if cancellation_polls >= 50:
                        try:
                            os.kill(child, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
            time.sleep(0.01)

        chunks = []
        while True:
            chunk = os.read(read_fd, 4096)
            if not chunk:
                break
            chunks.append(chunk)
        os.close(read_fd)

        if cancellation_sent:
            result = PythonRunResult(
                PythonModuleStatus.INTERRUPTED,
                PythonModulePhase.EXECUTE,
                "Isolated module was cancelled",
            )
        elif os.WIFSIGNALED(child_status):
            message = f"Isolated module terminated by signal {os.WTERMSIG(child_status)}"
            result = PythonRunResult(
                PythonModuleStatus.FAILED,
                PythonModulePhase.EXECUTE,
                message,
                RuntimeError(message),
            )
        else:
            try:
                payload = json.loads(b"".join(chunks).decode("utf-8"))
                status = PythonModuleStatus(payload["status"])
                phase = PythonModulePhase(payload["phase"])
                message = str(payload.get("message", ""))
                result = PythonRunResult(
                    status,
                    phase,
                    message,
                    RuntimeError(message) if status is PythonModuleStatus.FAILED else None,
                )
            except Exception as error:
                message = f"Isolated module exited without a valid result: {error}"
                result = PythonRunResult(
                    PythonModuleStatus.FAILED,
                    PythonModulePhase.EXECUTE,
                    message,
                    RuntimeError(message),
                )
        return module.adopt_external_run_result(result)


class py_amcm:
    def __init__(self):
        self.ctrl = AMCM()
        self.modules = {}
        self.executed_modules = []
        self._module_name_counters = {}
        self.last_workflow_provenance_path = ""
        init_interrupt()

    def _register_python_plugin(self, class_name, instance_name):
        index = _load_python_plugin_index()
        info = index.get(class_name)
        if not info:
            raise RuntimeError(f"Module not found: {class_name}")
        mod = _import_python_plugin(info)
        cls = getattr(mod, info["class"])
        module_obj = cls()
        if not isinstance(module_obj, base_module):
            raise TypeError("Module must inherit from cascade.pymodule.base_module")
        module_obj.set_name(instance_name)
        handle = _PythonModuleHandle(module_obj)
        self.modules[instance_name] = handle
        log(log_level.INFO, "CONTROL", f"Module {module_obj.get_basename()} is registered as {instance_name}")
        return handle

    def register_module(self, class_name, name=None):
        if name is None:
            count = self._module_name_counters.get(class_name, 0)
            while True:
                count += 1
                instance_name = f"{class_name}_{count}"
                if instance_name not in self.modules:
                    break
            self._module_name_counters[class_name] = count
        else:
            instance_name = name
        if instance_name in self.modules:
            raise RuntimeError(f"Module instance already registered: {instance_name}")
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
        if name in self.modules:
            raise RuntimeError(f"Module instance already registered: {name}")
        if not isinstance(module_obj, base_module):
            raise TypeError("Module must inherit from cascade.pymodule.base_module")
        modname = module_obj.__class__.__module__
        if not modname.startswith("cascade.pyplugin."):
            raise RuntimeError(f"Python module {modname} is not a signed cascade.pyplugin module; refusing to register it.")
        info = _load_python_plugin_index().get(module_obj.__class__.__name__)
        if not info or os.path.realpath(info["path"]) != os.path.realpath(sys.modules[modname].__file__):
            raise RuntimeError(f"Python module {module_obj.__class__.__name__} is not present in the verified plugin index.")
        module_obj.set_name(name)
        handle = _PythonModuleHandle(module_obj)
        self.modules[name] = handle
        return handle

    def run_module(self, name_or_mod, isolated=False):
        if isinstance(name_or_mod, str):
            handle = self.modules.get(name_or_mod)
            if handle is None:
                raise RuntimeError(f"Module not registered: {name_or_mod}")
        elif isinstance(name_or_mod, (_CppModuleHandle, _PythonModuleHandle)):
            handle = name_or_mod
        elif isinstance(name_or_mod, base_module):
            raise RuntimeError("Direct Python module execution is disabled; register a signed cascade.pyplugin module by name.")
        else:
            raise TypeError(f"Unsupported argument type: {type(name_or_mod)}")
        result = handle.run_isolated() if isolated else handle.run()
        phase = _status_text(result.phase)
        status = _status_text(result.status)
        manifest_path = handle.get_last_provenance_path()
        self.executed_modules.append({
            "run_id": handle.get_run_id(),
            "manifest_path": manifest_path,
            "name": handle.name(),
            "module": handle.get_basename(),
            "language": handle.language,
            "status": status,
            "phase": phase,
            "message": result.message,
        })
        return result

    def run_module_isolated(self, name_or_mod):
        return self.run_module(name_or_mod, isolated=True)

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
                progress[name] = dict(handle._module.get_progress())
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

    def add_module_to_dag(self, name, dependencies=None, isolated=False):
        if name not in self.modules:
            raise RuntimeError(f"Module not registered: {name}")
        dependencies = list(dependencies or [])

        def run_checked():
            result = self.run_module(name, isolated=isolated)
            if not result.allows_dependents():
                detail = f": {result.message}" if result.message else ""
                raise RuntimeError(
                    f"Module {name} finished with status {result.status}{detail}"
                )

        self.ctrl.get_dag().add_node(name, dependencies, run_checked)

    def link_dag_parameter(self, from_node, from_key, to_node, to_key):
        source = self.modules.get(from_node)
        target = self.modules.get(to_node)
        if source is None:
            raise RuntimeError(f"Module not registered: {from_node}")
        if target is None:
            raise RuntimeError(f"Module not registered: {to_node}")
        if from_key not in source.get_parameters():
            raise RuntimeError(
                f"DAG source parameter is not registered: {from_node}.{from_key}"
            )
        if to_key not in target.get_parameters():
            raise RuntimeError(
                f"DAG target parameter is not registered: {to_node}.{to_key}"
            )

        def transfer():
            value = copy.deepcopy(source.get_parameters()[from_key])
            target.set_param(to_key, value)

        label = f"{from_key} -> {to_key}"
        self.ctrl.get_dag().add_data_link(from_node, to_node, label, transfer)

    def run_dag(self, fail_fast=True, provenance_path=None):
        self.executed_modules.clear()
        result = self.ctrl.run_dag(fail_fast)
        self.last_workflow_provenance_path = self.save_provenance(
            provenance_path, fail_fast=fail_fast, dag_result=result
        )
        return result

    def save_run_log(self):
        return self.save_provenance()

    def run_group(self, group, fail_fast=True):
        if isinstance(group, (list, tuple)):
            results = []
            for mod in group:
                result = self.run_module(mod)
                results.append(result)
                if fail_fast and not result.allows_dependents():
                    break
            return results
        else:
            raise TypeError(f"Unsupported argument type: {type(group)}")

    def save_provenance(self, path=None, fail_fast=True, dag_result=None):
        workflow_id = (
            f"workflow-{os.getpid()}-{time.time_ns()}-"
            f"{hashlib.sha256(os.urandom(16)).hexdigest()[:8]}"
        )
        manifests = []
        latest_by_instance = {}
        for entry in self.executed_modules:
            manifest_path = entry.get("manifest_path", "")
            if not manifest_path or not os.path.isfile(manifest_path):
                continue
            with open(manifest_path, "r", encoding="utf-8") as source:
                manifest = json.load(source)
            if (
                manifest.get("schema") != "cascade.module-run"
                or manifest.get("schema_version") != 1
            ):
                raise RuntimeError(
                    f"Unsupported module provenance manifest: {manifest_path}"
                )
            manifests.append(manifest)
            latest_by_instance[entry["name"]] = manifest

        dag = self.ctrl.get_dag()
        dag_nodes = list(dag_result.nodes) if dag_result is not None else list(dag.get_node_results())
        dependencies = dag.get_dependencies()
        nodes = []
        if dag_nodes:
            for result in dag_nodes:
                module_manifest = latest_by_instance.get(result.name)
                nodes.append({
                    "name": result.name,
                    "status": _status_text(result.status),
                    "message": result.message,
                    "dependencies": list(dependencies.get(result.name, [])),
                    "module_run_id": (
                        module_manifest.get("run_id") if module_manifest else None
                    ),
                    "module_manifest": (
                        module_manifest.get("manifest_path") if module_manifest else None
                    ),
                })
        else:
            for entry in self.executed_modules:
                nodes.append({
                    "name": entry["name"],
                    "status": entry["status"],
                    "message": entry["message"],
                    "dependencies": [],
                    "module_run_id": entry["run_id"],
                    "module_manifest": entry["manifest_path"] or None,
                })

        data_links = [
            {
                "from": link.from_node,
                "to": link.to_node,
                "label": link.label,
            }
            for link in dag.get_data_links()
        ]
        starts = [
            manifest.get("timing", {}).get("started_at", "")
            for manifest in manifests
            if manifest.get("timing", {}).get("started_at")
        ]
        finishes = [
            manifest.get("timing", {}).get("finished_at", "")
            for manifest in manifests
            if manifest.get("timing", {}).get("finished_at")
        ]
        languages = {
            manifest.get("runtime", {}).get("language", "")
            for manifest in manifests
            if manifest.get("runtime", {}).get("language")
        }
        language = "mixed" if len(languages) > 1 else next(iter(languages), "python")
        abi_tag = getattr(cascade, "get_abi_tag", lambda: "")()
        root_version = ""
        for field in abi_tag.split(";"):
            if field.startswith("root="):
                root_version = field.partition("=")[2]
                break
        succeeded = all(node["status"] in {"Done", "Skipped", "Succeeded"} for node in nodes)
        target = path
        if not target:
            cache_root = os.getenv(
                "CASCADE_CACHE_DIR",
                os.path.join(os.path.expanduser("~"), ".cache", "cascade"),
            )
            target = os.path.join(
                cache_root, "provenance", "workflows", f"{workflow_id}.json"
            )
        target = os.path.abspath(os.path.expanduser(target))
        document = {
            "schema": "cascade.workflow-run",
            "schema_version": 1,
            "run_id": workflow_id,
            "timing": {
                "started_at": min(starts) if starts else _utc_now(),
                "finished_at": max(finishes) if finishes else _utc_now(),
            },
            "runtime": {
                "cascade_version": getattr(cascade, "__version__", ""),
                "plugin_abi_version": int(getattr(cascade, "__abi_version__", 1)),
                "plugin_abi_tag": abi_tag,
                "root_version": root_version,
                "language": language,
            },
            "execution": {
                "fail_fast": bool(fail_fast),
                "succeeded": succeeded,
            },
            "dag": {
                "nodes": nodes,
                "data_links": data_links,
            },
            "module_manifests": [
                manifest.get("manifest_path", "")
                for manifest in manifests
                if manifest.get("manifest_path")
            ],
            "manifest_path": target,
        }
        saved = _atomic_write_json(target, document)
        self.last_workflow_provenance_path = saved
        log(log_level.INFO, "CONTROL", f"Workflow provenance '{saved}' is saved.")
        return saved

    def save_run_log_all(self, log_dir=None):
        if log_dir:
            workflow_id = f"workflow-{os.getpid()}-{time.time_ns()}"
            return self.save_provenance(
                os.path.join(log_dir, f"{workflow_id}.json")
            )
        return self.save_provenance()
