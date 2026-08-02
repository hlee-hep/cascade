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
from datetime import datetime, timezone
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


def _utc_now():
    return datetime.now(timezone.utc).isoformat(timespec="microseconds").replace("+00:00", "Z")


def _sensitive_key(key):
    key = str(key).lower()
    return any(
        pattern in key
        for pattern in (
            "password",
            "passwd",
            "secret",
            "token",
            "credential",
            "private_key",
            "api_key",
        )
    )


def _sanitized_parameters(parameters):
    return {
        key: "***" if _sensitive_key(key) else value
        for key, value in parameters.items()
    }


def _sha256_path(path):
    digest = hashlib.sha256()
    with open(path, "rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _capture_artifact(source, recorded_path):
    source_text = str(source)
    if "://" in source_text:
        return {
            "path": recorded_path,
            "kind": "uri",
            "exists": False,
            "size": 0,
            "sha256": None,
        }
    path = Path(source).expanduser()
    if not path.is_absolute():
        path = Path.cwd() / path
    path = Path(os.path.abspath(path))
    if not path.exists() and not path.is_symlink():
        return {
            "path": recorded_path,
            "kind": "missing",
            "exists": False,
            "size": 0,
            "sha256": None,
        }
    if path.is_symlink():
        target = os.readlink(path)
        return {
            "path": recorded_path,
            "kind": "symlink",
            "exists": True,
            "size": len(target),
            "sha256": hashlib.sha256(target.encode("utf-8")).hexdigest(),
        }
    if path.is_file():
        return {
            "path": recorded_path,
            "kind": "file",
            "exists": True,
            "size": path.stat().st_size,
            "sha256": _sha256_path(path),
        }
    if not path.is_dir():
        return {
            "path": recorded_path,
            "kind": "other",
            "exists": True,
            "size": 0,
            "sha256": None,
        }

    digest = hashlib.sha256()
    total_size = 0
    for entry in sorted(path.rglob("*"), key=lambda item: item.as_posix()):
        relative = entry.relative_to(path).as_posix()
        if entry.is_symlink():
            target = os.readlink(entry)
            size = len(target)
            fingerprint = hashlib.sha256(target.encode("utf-8")).hexdigest()
            marker = "l"
        elif entry.is_dir():
            size = 0
            fingerprint = ""
            marker = "d"
        elif entry.is_file():
            size = entry.stat().st_size
            fingerprint = _sha256_path(entry)
            marker = "f"
        else:
            continue
        total_size += size
        digest.update(
            f"{marker}\0{relative}\0{fingerprint}\0{size}\0".encode("utf-8")
        )
    return {
        "path": recorded_path,
        "kind": "directory",
        "exists": True,
        "size": total_size,
        "sha256": digest.hexdigest(),
    }


def _atomic_write_json(path, document):
    path = Path(path).expanduser().resolve(strict=False)
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=path.parent, text=True
    )
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as output:
            json.dump(document, output, indent=2, sort_keys=False)
            output.write("\n")
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.remove(temporary)


def _runtime_provenance(language):
    abi_tag = getattr(cascade, "get_abi_tag", lambda: "")()
    root_version = ""
    for field in abi_tag.split(";"):
        if field.startswith("root="):
            root_version = field.partition("=")[2]
            break
    return {
        "cascade_version": getattr(cascade, "__version__", ""),
        "plugin_abi_version": int(getattr(cascade, "__abi_version__", 1)),
        "plugin_abi_tag": abi_tag,
        "root_version": root_version,
        "language": language,
    }


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

    def staged_outputs(self):
        with self._lock:
            return list(self._staged.items())


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
        self._provenance_started_at = ""
        self._provenance_isolated = False
        self._provenance_inputs = []
        self._provenance_cache_source = ""
        self._snapshot_hash = ""
        self._last_provenance = None
        self._pending_cache_provenance = ""
        self._plugin_origin = None

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

    def set_plugin_origin(self, origin):
        if origin is not None and not isinstance(origin, dict):
            raise TypeError("Plugin origin must be a mapping or None")
        self._plugin_origin = dict(origin) if origin is not None else None

    def stage_output(self, path):
        return self.context.stage_output(path)

    def final_output(self, path):
        return self.context.final_output(path)

    def track_input(self, path):
        if not self.context.active:
            raise RuntimeError("Cannot track an input outside an active module run")
        value = str(path)
        if value not in self._provenance_inputs:
            self._provenance_inputs.append(value)

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

    def get_run_id(self):
        return self.context.run_id

    def get_last_provenance_path(self):
        return self._last_provenance.get("manifest_path", "") if self._last_provenance else ""

    def get_last_provenance_json(self, indent=2):
        return json.dumps(self._last_provenance, indent=indent) if self._last_provenance else ""

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
            self._begin_provenance(True)
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
                    self._begin_provenance(False)
                self.init()
            except Exception as exc:
                return self._fail(ModulePhase.INIT, exc)

            if self.check_interrupt():
                return self._finish(ModuleStatus.INTERRUPTED, ModulePhase.INIT, "Interrupted after initialization")

            try:
                if self.params.get("dry_run", False):
                    return self._finish(ModuleStatus.SKIPPED, ModulePhase.CHECK, "dry_run enabled")

                self._snapshot_hash = self._compute_snapshot_hash()
                if not self.params.get("force_run", False) and self._is_hash_cached(self._snapshot_hash):
                    self._provenance_cache_source = self._find_cached_provenance(self._snapshot_hash)
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
                provenance_path = self._successful_provenance_path()
                manifest = self._build_provenance(
                    RunResult(ModuleStatus.DONE, ModulePhase.NONE, ""),
                    self.context.outputs.staged_outputs(),
                    provenance_path,
                )
                staged_manifest = self.stage_output(provenance_path)
                _atomic_write_json(staged_manifest, manifest)
                self.context.outputs.commit()
                self._pending_cache_provenance = provenance_path
                self._save_hash_cache(self._snapshot_hash)
                try:
                    self.context.complete_run()
                except Exception:
                    self._remove_hash_cache(self._snapshot_hash)
                    raise
                self._last_provenance = manifest
            except Exception as exc:
                return self._fail(ModulePhase.COMMIT, exc)
            return self._finish(ModuleStatus.DONE, ModulePhase.NONE, "")

    def _finish(self, status, phase, message, exception=None):
        if status is not ModuleStatus.DONE and self.context.active:
            self.context.rollback_run()
        self.set_status(status)
        self.last_run_result = RunResult(status, phase, message, exception)
        self._finalize_provenance(self.last_run_result)
        return self.last_run_result

    def _begin_provenance(self, isolated):
        self._provenance_started_at = _utc_now()
        self._provenance_isolated = bool(isolated)
        self._provenance_inputs = []
        self._provenance_cache_source = ""
        self._snapshot_hash = ""
        self._last_provenance = None
        self._pending_cache_provenance = ""

    def _successful_provenance_path(self):
        return str(
            self.context.output_directory
            / ".cascade"
            / "provenance"
            / "modules"
            / f"{self.context.run_id}.json"
        )

    def _terminal_provenance_path(self):
        return str(
            self.context.cache_directory
            / "provenance"
            / "modules"
            / f"{self.context.run_id}.json"
        )

    def _build_provenance(self, result, staged_outputs, manifest_path):
        outputs = []
        for final, staged in staged_outputs:
            try:
                recorded_path = final.relative_to(self.context.output_directory).as_posix()
            except ValueError:
                recorded_path = str(final)
            outputs.append(_capture_artifact(staged, recorded_path))
        return {
            "schema": "cascade.module-run",
            "schema_version": 1,
            "run_id": self.context.run_id,
            "module": {
                "instance": self.name(),
                "name": self.get_basename(),
                "metadata": self.get_metadata(),
            },
            "runtime": _runtime_provenance("python"),
            "plugin": self._plugin_origin,
            "identity": {
                "code_hash": self.code_version_hash,
                "snapshot_hash": self._snapshot_hash,
            },
            "parameters": _sanitized_parameters(dict(self.params)),
            "timing": {
                "started_at": self._provenance_started_at or _utc_now(),
                "finished_at": _utc_now(),
            },
            "directories": {
                "output": str(self.context.output_directory),
                "cache": str(self.context.cache_directory),
            },
            "execution": {
                "isolated": self._provenance_isolated,
                "cache_hit": (
                    result.status is ModuleStatus.SKIPPED
                    and result.message == "snapshot already cached"
                ),
                "dry_run": (
                    result.status is ModuleStatus.SKIPPED
                    and result.message == "dry_run enabled"
                ),
                "cache_source_manifest": self._provenance_cache_source or None,
            },
            "result": {
                "status": result.status.value,
                "phase": result.phase.value,
                "message": result.message,
            },
            "artifacts": {
                "inputs": [
                    _capture_artifact(path, path)
                    for path in self._provenance_inputs
                ],
                "outputs": outputs,
            },
            "manifest_path": str(Path(manifest_path).expanduser().resolve(strict=False)),
        }

    def _finalize_provenance(self, result):
        try:
            if (
                self._last_provenance
                and self._last_provenance.get("run_id") == self.context.run_id
            ):
                return
            expected = (
                self._successful_provenance_path()
                if result.status is ModuleStatus.DONE
                else self._terminal_provenance_path()
            )
            if os.path.isfile(expected):
                with open(expected, "r", encoding="utf-8") as source:
                    existing = json.load(source)
                existing_result = existing.get("result", {})
                if (
                    existing_result.get("status") == result.status.value
                    and existing_result.get("phase") == result.phase.value
                ):
                    self._last_provenance = existing
                    return
            manifest = self._build_provenance(result, [], expected)
            _atomic_write_json(expected, manifest)
            self._last_provenance = manifest
        except Exception as error:
            log(log_level.ERROR, self.m_name, f"Failed to record provenance: {error}")

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
            return {}
        with open(path, "r", encoding="utf-8") as cache:
            data = json.load(cache)
        if isinstance(data, list) and all(isinstance(item, str) for item in data):
            return {item: "" for item in data}
        if not isinstance(data, dict) or not isinstance(data.get("snapshots"), list):
            raise RuntimeError(f"Python snapshot cache has an invalid schema: {path}")
        if data.get("schema_version") != 1:
            raise RuntimeError(
                f"Python snapshot cache has an unsupported schema version: {path}"
            )
        result = {}
        for entry in data["snapshots"]:
            if not isinstance(entry, dict) or not isinstance(entry.get("hash"), str):
                raise RuntimeError(f"Python snapshot cache has an invalid entry: {path}")
            result[entry["hash"]] = str(entry.get("provenance") or "")
        return result

    @staticmethod
    def _write_hashes(hashes):
        return {
            "schema_version": 1,
            "snapshots": [
                {"hash": key, "provenance": hashes[key]}
                for key in sorted(hashes)
            ],
        }

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

    def _find_cached_provenance(self, snapshot_hash):
        with base_module._cache_lock:
            path = self._hash_cache_path()
            lock_path = path + ".lock"
            with open(lock_path, "a+", encoding="utf-8") as lock:
                fcntl.flock(lock.fileno(), fcntl.LOCK_SH)
                try:
                    return self._read_hashes(path).get(snapshot_hash, "")
                finally:
                    fcntl.flock(lock.fileno(), fcntl.LOCK_UN)

    def _save_hash_cache(self, snapshot_hash):
        with base_module._cache_lock:
            path = self._hash_cache_path()
            lock_path = path + ".lock"
            with open(lock_path, "a+", encoding="utf-8") as lock:
                fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
                existing = self._read_hashes(path)
                existing[snapshot_hash] = self._pending_cache_provenance
                descriptor, temporary = tempfile.mkstemp(prefix=".cascade-cache-", dir=os.path.dirname(path), text=True)
                try:
                    with os.fdopen(descriptor, "w", encoding="utf-8") as output:
                        json.dump(self._write_hashes(existing), output, indent=2)
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
                existing.pop(snapshot_hash, None)
                descriptor, temporary = tempfile.mkstemp(
                    prefix=".cascade-cache-", dir=os.path.dirname(path), text=True
                )
                try:
                    with os.fdopen(descriptor, "w", encoding="utf-8") as output:
                        json.dump(self._write_hashes(existing), output, indent=2)
                        output.flush()
                        os.fsync(output.fileno())
                    os.replace(temporary, path)
                finally:
                    if os.path.exists(temporary):
                        os.remove(temporary)
                fcntl.flock(lock.fileno(), fcntl.LOCK_UN)

    def snapshot_state(self):
        return {}
