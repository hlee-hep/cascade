from cascade import is_interrupted, log, log_level
import cascade
import hashlib
import json
import os
import shutil
import threading
import tempfile
import time
import uuid
import fcntl
from dataclasses import dataclass
from enum import Enum
from pathlib import Path


class ModuleStatus(str, Enum):
    PENDING = "Pending"
    INITIALIZING = "Initializing"
    RUNNING = "Running"
    FINALIZING = "Finalizing"
    DONE = "Done"
    SKIPPED = "Skipped"
    INTERRUPTED = "Interrupted"
    FAILED = "Failed"


class ModulePhase(str, Enum):
    NONE = "None"
    INIT = "Init"
    CHECK = "Check"
    EXECUTE = "Execute"
    FINALIZE = "Finalize"
    COMMIT = "Commit"


@dataclass(frozen=True)
class RunResult:
    status: ModuleStatus = ModuleStatus.PENDING
    phase: ModulePhase = ModulePhase.NONE
    message: str = ""
    exception: BaseException = None

    def succeeded(self):
        return self.status is ModuleStatus.DONE

    def failed(self):
        return self.status is ModuleStatus.FAILED

    def is_terminal(self):
        return self.status in {
            ModuleStatus.DONE,
            ModuleStatus.SKIPPED,
            ModuleStatus.INTERRUPTED,
            ModuleStatus.FAILED,
        }

    def allows_dependents(self):
        return self.status in {ModuleStatus.DONE, ModuleStatus.SKIPPED}


class CancellationToken:
    def __init__(self):
        self._requested = threading.Event()

    def request(self):
        self._requested.set()

    def reset(self):
        self._requested.clear()

    def is_cancellation_requested(self):
        return self._requested.is_set() or is_interrupted()


class OutputTransaction:
    def __init__(self):
        self._output_root = None
        self._staging_root = None
        self._staged = {}
        self._promotions = []
        self._state = "idle"
        self._lock = threading.RLock()

    @staticmethod
    def _absolute(path):
        return Path(path).expanduser().resolve(strict=False)

    @staticmethod
    def _contained(root, candidate):
        try:
            return candidate != root and candidate.is_relative_to(root)
        except AttributeError:
            try:
                candidate.relative_to(root)
                return candidate != root
            except ValueError:
                return False

    def begin(self, output_root, run_id):
        with self._lock:
            self.rollback()
            self._output_root = self._absolute(output_root)
            self._staging_root = self._output_root / ".cascade-staging" / run_id
            self._staged = {}
            self._promotions = []
            self._state = "staging"

    def final_path(self, path):
        with self._lock:
            candidate = Path(path)
            final = self._absolute(candidate if candidate.is_absolute() else self._output_root / candidate)
            if not self._contained(self._output_root, final):
                raise RuntimeError(
                    f"OutputTransaction: output path must be inside the configured output directory: {final}"
                )
            return final

    def stage(self, path):
        with self._lock:
            if self._state != "staging":
                raise RuntimeError("OutputTransaction: no active staging transaction")
            final = self.final_path(path)
            staged = self._staging_root / "files" / final.relative_to(self._output_root)
            staged.parent.mkdir(parents=True, exist_ok=True)
            for existing in self._staged:
                if existing == final:
                    continue
                if self._contained(existing, final) or self._contained(final, existing):
                    raise RuntimeError(
                        f"OutputTransaction: staged outputs cannot overlap: {existing} and {final}"
                    )
            self._staged[final] = staged
            return staged

    def commit(self):
        with self._lock:
            if self._state != "staging":
                raise RuntimeError("OutputTransaction: transaction is not ready to commit")
            try:
                for final, staged in sorted(self._staged.items(), key=lambda item: str(item[0])):
                    if not staged.exists():
                        raise RuntimeError(f"OutputTransaction: staged output was not created: {staged}")
                    backup = self._staging_root / "backups" / final.relative_to(self._output_root)
                    had_original = final.exists()
                    self._promotions.append((final, staged, backup, had_original))

                if self._promotions:
                    self._staging_root.mkdir(parents=True, exist_ok=True)
                    journal = self._staging_root / "promotion-journal.json"
                    descriptor, temporary = tempfile.mkstemp(
                        prefix=".promotion-journal-", dir=self._staging_root, text=True
                    )
                    try:
                        with os.fdopen(descriptor, "w", encoding="utf-8") as output:
                            json.dump(
                                [
                                    {
                                        "final": str(final),
                                        "staged": str(staged),
                                        "backup": str(backup),
                                        "had_original": had_original,
                                    }
                                    for final, staged, backup, had_original in self._promotions
                                ],
                                output,
                            )
                            output.flush()
                            os.fsync(output.fileno())
                        os.replace(temporary, journal)
                    finally:
                        if os.path.exists(temporary):
                            os.remove(temporary)

                for final, staged, backup, had_original in self._promotions:
                    final.parent.mkdir(parents=True, exist_ok=True)
                    if had_original:
                        backup.parent.mkdir(parents=True, exist_ok=True)
                        os.replace(final, backup)
                    os.replace(staged, final)
                self._state = "promoted"
            except Exception:
                self.rollback()
                raise

    def complete(self):
        with self._lock:
            if self._state not in {"staging", "promoted"}:
                raise RuntimeError("OutputTransaction: transaction cannot be completed in its current state")
            if self._staging_root and self._staging_root.exists():
                shutil.rmtree(self._staging_root, ignore_errors=False)
                parent = self._staging_root.parent
                if parent.exists() and not any(parent.iterdir()):
                    parent.rmdir()
            self._staged = {}
            self._promotions = []
            self._state = "completed"

    def rollback(self):
        with self._lock:
            promotions = list(self._promotions)
            journal = self._staging_root / "promotion-journal.json" if self._staging_root else None
            if not promotions and journal and journal.exists():
                try:
                    data = json.loads(journal.read_text(encoding="utf-8"))
                    for entry in data:
                        final = self._absolute(entry["final"])
                        staged = self._absolute(entry["staged"])
                        backup = self._absolute(entry["backup"]) if entry["backup"] else Path()
                        had_original = bool(entry["had_original"])
                        if not self._contained(self._output_root, final):
                            continue
                        if not self._contained(self._staging_root, staged):
                            continue
                        if had_original and (not entry["backup"] or not self._contained(self._staging_root, backup)):
                            continue
                        promotions.append((final, staged, backup, had_original))
                except Exception:
                    promotions = []
            for final, staged, backup, had_original in reversed(promotions):
                try:
                    if had_original:
                        if not backup.exists():
                            continue
                        if final.is_dir():
                            shutil.rmtree(final)
                        elif final.exists():
                            final.unlink()
                        final.parent.mkdir(parents=True, exist_ok=True)
                        os.replace(backup, final)
                    elif not staged.exists():
                        if final.is_dir():
                            shutil.rmtree(final)
                        elif final.exists():
                            final.unlink()
                except OSError:
                    pass
            if self._staging_root:
                shutil.rmtree(self._staging_root, ignore_errors=True)
                parent = self._staging_root.parent
                try:
                    if parent.exists() and not any(parent.iterdir()):
                        parent.rmdir()
                except OSError:
                    pass
            self._staged = {}
            self._promotions = []
            if self._state not in {"idle", "completed"}:
                self._state = "rolled_back"

    @property
    def active(self):
        with self._lock:
            return self._state in {"staging", "promoted"}


class ExecutionContext:
    def __init__(self):
        cache_default = os.path.join(os.path.expanduser("~"), ".cache", "cascade", "snapshot_cache")
        self.cache_directory = Path(os.getenv("CASCADE_CACHE_DIR", cache_default)).expanduser().resolve(strict=False)
        self.output_directory = Path(os.getenv("CASCADE_OUTPUT_DIR", os.getcwd())).expanduser().resolve(strict=False)
        self.run_id = ""
        self.cancellation = CancellationToken()
        self.outputs = OutputTransaction()
        self._active = False
        self._lock = threading.RLock()

    def begin_run(self, instance_name, module_name):
        with self._lock:
            if self._active:
                raise RuntimeError("ExecutionContext: a run is already active")
            safe_name = "".join(
                character if character.isalnum() or character in "-_" else "_"
                for character in (instance_name or module_name or "unnamed")
            )
            self.run_id = f"{safe_name}-{os.getpid()}-{time.time_ns()}-{uuid.uuid4().hex[:8]}"
            self.cancellation.reset()
            self.outputs.begin(self.output_directory, self.run_id)
            self._active = True

    def complete_run(self):
        with self._lock:
            self.outputs.complete()
            self._active = False

    def rollback_run(self):
        with self._lock:
            self.outputs.rollback()
            self._active = False

    def cleanup_external_run(self):
        self.rollback_run()

    def set_cache_directory(self, path):
        with self._lock:
            if self._active:
                raise RuntimeError("ExecutionContext: cannot change the cache directory during a run")
            self.cache_directory = Path(path).expanduser().resolve(strict=False)

    def set_output_directory(self, path):
        with self._lock:
            if self._active:
                raise RuntimeError("ExecutionContext: cannot change the output directory during a run")
            self.output_directory = Path(path).expanduser().resolve(strict=False)

    def stage_output(self, path):
        return self.outputs.stage(path)

    def final_output(self, path):
        return self.outputs.final_path(path)

    @property
    def active(self):
        with self._lock:
            return self._active

    def snapshot_state(self):
        return {"output_dir": str(self.output_directory)}


class TypedParameters(dict):
    def __init__(self):
        super().__init__()
        self._types = {}
        self._descriptions = {}

    @staticmethod
    def _descriptor(value):
        if value is None:
            return ("none",)
        if isinstance(value, bool):
            return ("bool",)
        if isinstance(value, int):
            return ("int",)
        if isinstance(value, float):
            return ("float",)
        if isinstance(value, str):
            return ("string",)
        if isinstance(value, list):
            if not value:
                return ("list", "mixed")
            element_types = {TypedParameters._descriptor(item)[0] for item in value}
            if not element_types <= {"bool", "int", "float", "string"}:
                raise TypeError("Python module parameters do not support nested lists or objects")
            if element_types == {"int"}:
                return ("list", "int")
            if element_types <= {"int", "float"}:
                return ("list", "float")
            if element_types == {"string"}:
                return ("list", "string")
            return ("list", "mixed")
        raise TypeError(f"Unsupported Python module parameter type: {type(value).__name__}")

    @staticmethod
    def _coerce(name, descriptor, value):
        kind = descriptor[0]
        if kind == "none":
            if value is None:
                return None
        elif kind == "bool":
            if isinstance(value, bool):
                return value
        elif kind == "int":
            if isinstance(value, int) and not isinstance(value, bool):
                return value
        elif kind == "float":
            if isinstance(value, (int, float)) and not isinstance(value, bool):
                return float(value)
        elif kind == "string":
            if isinstance(value, str):
                return value
        elif kind == "list" and isinstance(value, list):
            element_kind = descriptor[1]
            if element_kind == "int" and all(isinstance(item, int) and not isinstance(item, bool) for item in value):
                return list(value)
            if element_kind == "float" and all(isinstance(item, (int, float)) and not isinstance(item, bool) for item in value):
                return [float(item) for item in value]
            if element_kind == "string" and all(isinstance(item, str) for item in value):
                return list(value)
            if element_kind == "mixed":
                TypedParameters._descriptor(value)
                return list(value)
        raise TypeError(f"Parameter '{name}' expects {descriptor}, got {type(value).__name__}")

    def register(self, name, default, description=""):
        if name in self._types:
            raise KeyError(f"Parameter already registered: {name}")
        descriptor = self._descriptor(default)
        self._types[name] = descriptor
        self._descriptions[name] = description
        dict.__setitem__(self, name, self._coerce(name, descriptor, default))

    def __setitem__(self, name, value):
        if name not in self._types:
            self.register(name, value)
            return
        dict.__setitem__(self, name, self._coerce(name, self._types[name], value))

    def set_registered(self, name, value):
        if name not in self._types:
            raise KeyError(f"Parameter is not registered: {name}")
        self[name] = value


class base_module:
    _cache_lock = threading.Lock()

    def __init__(self):
        self.params = TypedParameters()
        self.params.register("dry_run", False, "simulate execution only")
        self.params.register("force_run", False, "force execution")
        self.status = ModuleStatus.PENDING
        self.last_run_result = RunResult()
        self._run_lock = threading.RLock()
        self.code_version_hash = ""
        self.basename = ""
        self.m_name = ""
        self.summary = ""
        self.tags = []
        self.context = ExecutionContext()
        self._external_run_reserved = False

    def check_interrupt(self):
        if self.context.cancellation.is_cancellation_requested():
            self.set_status("Interrupted")
            return True
        return False

    def request_cancellation(self):
        self.context.cancellation.request()

    def is_cancellation_requested(self):
        return self.context.cancellation.is_cancellation_requested()

    def set_cache_directory(self, path):
        with self._run_lock:
            self.context.set_cache_directory(path)

    def set_output_directory(self, path):
        with self._run_lock:
            self.context.set_output_directory(path)

    def stage_output(self, path):
        return self.context.stage_output(path)

    def final_output(self, path):
        return self.context.final_output(path)

    def set_param(self, key, val):
        with self._run_lock:
            self.params.set_registered(key, val)

    def register_param(self, key, default, description=""):
        with self._run_lock:
            self.params.register(key, default, description)
    
    def set_param_from_yaml(self, path):
        import yaml
        with self._run_lock, open(path) as f:
            data = yaml.safe_load(f) or {}
            for k, v in data.items():
                self.params.set_registered(k, v)

    def get_param(self, key):
        with self._run_lock:
            return self.params.get(key, None)

    def set_status(self, status):
        if not isinstance(status, ModuleStatus):
            status = ModuleStatus(status)
        self.status = status
        log(log_level.INFO, self.m_name, "Status : " + status.value)

    def get_status(self):
        return self.status.value

    def get_last_run_result(self):
        return self.last_run_result

    def set_name(self, name):
        self.m_name = name

    def name(self):
        return self.m_name

    def get_basename(self):
        return self.basename

    def get_parameters(self):
        with self._run_lock:
            return dict(self.params)

    def get_code_hash(self):
        return self.code_version_hash

    def get_progress(self):
        return {}

    def get_metadata(self):
        declared = getattr(self.__class__, "METADATA", None)
        if isinstance(declared, dict):
            return {
                "name": declared.get("name", self.get_basename() or self.__class__.__name__),
                "version": declared.get("version", ""),
                "summary": declared.get("summary", ""),
                "tags": list(declared.get("tags", [])),
            }
        return {
            "name": self.get_basename() or self.__class__.__name__,
            "version": getattr(self.__class__, "VERSION", getattr(cascade, "__version__", "")),
            "summary": getattr(self.__class__, "SUMMARY", self.summary),
            "tags": list(getattr(self.__class__, "TAGS", self.tags)),
        }

    def print_description(self):
        raise NotImplementedError("PythonModuleBase: print_description() must be implemented by subclass")

    def run(self):
        return self._run_impl(False)

    def prepare_external_run(self):
        with self._run_lock:
            if self._external_run_reserved or self.context.active:
                raise RuntimeError(f"Module run is already active: {self.name()}")
            self.context.begin_run(self.name(), self.get_basename())
            self._external_run_reserved = True
            self.set_status(ModuleStatus.INITIALIZING)

    def run_prepared_external(self):
        return self._run_impl(True)

    def adopt_external_run_result(self, result):
        with self._run_lock:
            if not self._external_run_reserved:
                raise RuntimeError(f"Module has no reserved external run: {self.name()}")
            self._external_run_reserved = False
            self.context.cleanup_external_run()
            if result.failed() and result.exception is None:
                result = RunResult(result.status, result.phase, result.message, RuntimeError(result.message))
            return self._finish(result.status, result.phase, result.message, result.exception)

    def _run_impl(self, external_prepared):
        with self._run_lock:
            if self._external_run_reserved != external_prepared:
                if external_prepared:
                    raise RuntimeError(f"Module has no reserved external run: {self.name()}")
                raise RuntimeError(f"Module is reserved for isolated execution: {self.name()}")
            if external_prepared:
                self._external_run_reserved = False
            self.set_status(ModuleStatus.INITIALIZING)
            try:
                if not self.context.active:
                    self.context.begin_run(self.name(), self.get_basename())
                self.init()
            except Exception as exc:
                return self._fail(ModulePhase.INIT, exc)

            if self.check_interrupt():
                return self._finish(ModuleStatus.INTERRUPTED, ModulePhase.INIT, "Interrupted after initialization")

            try:
                if self.params.get("dry_run", False):
                    return self._finish(ModuleStatus.SKIPPED, ModulePhase.CHECK, "dry_run enabled")

                snapshot_hash = self._compute_snapshot_hash()
                if not self.params.get("force_run", False) and self._is_hash_cached(snapshot_hash):
                    log(log_level.INFO, self.m_name, "Matching snapshot is already cached.")
                    return self._finish(ModuleStatus.SKIPPED, ModulePhase.CHECK, "snapshot already cached")
            except Exception as exc:
                return self._fail(ModulePhase.CHECK, exc)

            self.set_status(ModuleStatus.RUNNING)
            try:
                self.execute()
            except Exception as exc:
                if self.is_cancellation_requested():
                    return self._finish(ModuleStatus.INTERRUPTED, ModulePhase.EXECUTE, str(exc), exc)
                return self._fail(ModulePhase.EXECUTE, exc)

            if self.check_interrupt():
                return self._finish(ModuleStatus.INTERRUPTED, ModulePhase.EXECUTE, "Interrupted during execution")

            self.set_status(ModuleStatus.FINALIZING)
            try:
                self.finalize()
            except Exception as exc:
                return self._fail(ModulePhase.FINALIZE, exc)

            if self.check_interrupt():
                return self._finish(ModuleStatus.INTERRUPTED, ModulePhase.FINALIZE, "Interrupted during finalization")

            try:
                self.context.outputs.commit()
                self._save_hash_cache(snapshot_hash)
                try:
                    self.context.complete_run()
                except Exception:
                    self._remove_hash_cache(snapshot_hash)
                    raise
            except Exception as exc:
                return self._fail(ModulePhase.COMMIT, exc)
            return self._finish(ModuleStatus.DONE, ModulePhase.NONE, "")

    def _finish(self, status, phase, message, exception=None):
        if status is not ModuleStatus.DONE and self.context.active:
            self.context.rollback_run()
        self.set_status(status)
        self.last_run_result = RunResult(status, phase, message, exception)
        return self.last_run_result

    def _fail(self, phase, error):
        message = str(error) or error.__class__.__name__
        log(log_level.ERROR, self.m_name, f"Module failed during {phase.value}: {message}")
        self._invoke_failure_hook(phase, message)
        return self._finish(ModuleStatus.FAILED, phase, message, error)

    def _invoke_failure_hook(self, phase, message):
        try:
            self.on_failure(phase, message)
        except Exception as hook_error:
            log(log_level.ERROR, self.m_name, f"on_failure hook failed: {hook_error}")

    def init(self):
        raise NotImplementedError("PythonModuleBase: init() must be implemented by subclass")

    def execute(self):
        raise NotImplementedError("PythonModuleBase: execute() must be implemented by subclass")
    
    def finalize(self):
        raise NotImplementedError("PythonModuleBase: finalize() must be implemented by subclass")

    def on_failure(self, phase, message):
        pass

    def _compute_snapshot_hash(self):
        payload = {
            "basename": self.basename,
            "code_hash": self.code_version_hash,
            "params": self.params,
            "state": self.snapshot_state(),
            "execution": self.context.snapshot_state(),
        }
        raw = json.dumps(payload, sort_keys=True, default=str)
        return hashlib.sha256(raw.encode("utf-8")).hexdigest()

    def _hash_cache_path(self):
        base = str(self.context.cache_directory)
        os.makedirs(base, exist_ok=True)
        return os.path.join(base, "python_modules.json")

    @staticmethod
    def _read_hashes(path):
        if not os.path.exists(path):
            return set()
        with open(path, "r", encoding="utf-8") as cache:
            data = json.load(cache)
        if not isinstance(data, list) or not all(isinstance(item, str) for item in data):
            raise RuntimeError(f"Python snapshot cache must contain a JSON string list: {path}")
        return set(data)

    def _is_hash_cached(self, snapshot_hash):
        with base_module._cache_lock:
            path = self._hash_cache_path()
            lock_path = path + ".lock"
            with open(lock_path, "a+", encoding="utf-8") as lock:
                fcntl.flock(lock.fileno(), fcntl.LOCK_SH)
                try:
                    return snapshot_hash in self._read_hashes(path)
                finally:
                    fcntl.flock(lock.fileno(), fcntl.LOCK_UN)

    def _save_hash_cache(self, snapshot_hash):
        with base_module._cache_lock:
            path = self._hash_cache_path()
            lock_path = path + ".lock"
            with open(lock_path, "a+", encoding="utf-8") as lock:
                fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
                existing = self._read_hashes(path)
                if snapshot_hash in existing:
                    fcntl.flock(lock.fileno(), fcntl.LOCK_UN)
                    return
                existing.add(snapshot_hash)
                descriptor, temporary = tempfile.mkstemp(prefix=".cascade-cache-", dir=os.path.dirname(path), text=True)
                try:
                    with os.fdopen(descriptor, "w", encoding="utf-8") as output:
                        json.dump(sorted(existing), output, indent=2)
                        output.flush()
                        os.fsync(output.fileno())
                    os.replace(temporary, path)
                finally:
                    if os.path.exists(temporary):
                        os.remove(temporary)
                fcntl.flock(lock.fileno(), fcntl.LOCK_UN)

    def _remove_hash_cache(self, snapshot_hash):
        with base_module._cache_lock:
            path = self._hash_cache_path()
            lock_path = path + ".lock"
            with open(lock_path, "a+", encoding="utf-8") as lock:
                fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
                existing = self._read_hashes(path)
                existing.discard(snapshot_hash)
                descriptor, temporary = tempfile.mkstemp(
                    prefix=".cascade-cache-", dir=os.path.dirname(path), text=True
                )
                try:
                    with os.fdopen(descriptor, "w", encoding="utf-8") as output:
                        json.dump(sorted(existing), output, indent=2)
                        output.flush()
                        os.fsync(output.fileno())
                    os.replace(temporary, path)
                finally:
                    if os.path.exists(temporary):
                        os.remove(temporary)
                fcntl.flock(lock.fileno(), fcntl.LOCK_UN)

    def snapshot_state(self):
        return {}
