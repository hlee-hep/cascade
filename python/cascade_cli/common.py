import argparse
import importlib.util
import json
import os
import signal
import sys
from contextlib import contextmanager
from typing import Any, Dict, Optional, Tuple


_CLI_PREFIX = os.environ.get(
    "CASCADE_PREFIX",
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.realpath(__file__)))),
)
_CLI_PYTHON_ROOT = os.path.join(_CLI_PREFIX, "lib")
if os.path.isdir(os.path.join(_CLI_PYTHON_ROOT, "cascade")):
    sys.path.insert(0, _CLI_PYTHON_ROOT)


def _log(level: str, msg: str) -> None:
    stream = sys.stderr if level in ("WARN", "ERROR") else sys.stdout
    print(f"[{level}] {msg}", file=stream, flush=True)


def _install_sigint_handler() -> None:
    signal.signal(signal.SIGINT, signal.default_int_handler)


def _parse_kv(s: str) -> Tuple[str, Any]:
    if "=" not in s:
        raise argparse.ArgumentTypeError(f"--set expects k=v, got '{s}'")
    k, v = s.split("=", 1)
    k = k.strip()
    if not k:
        raise argparse.ArgumentTypeError("--set key cannot be empty")
    v = v.strip()
    if v.lower() in ("true", "false"):
        return k, v.lower() == "true"
    try:
        return k, int(v)
    except ValueError:
        pass
    try:
        return k, float(v)
    except ValueError:
        pass
    try:
        return k, json.loads(v)
    except Exception:
        pass
    return k, v


def _positive_int(value: str) -> int:
    parsed = int(value)
    if parsed < 1:
        raise argparse.ArgumentTypeError("value must be at least 1")
    return parsed


def _emit(data: Any, as_json: bool = False) -> None:
    if as_json:
        print(json.dumps(data, ensure_ascii=False, indent=2, sort_keys=True))
        return
    if isinstance(data, dict):
        for key, value in data.items():
            print(f"{key}: {value}")
        return
    print(data)


def _load_controller(quiet: bool = False, require_signed: bool = False):
    import cascade

    if quiet:
        cascade.set_log_level(cascade.log_level.NONE)
    from cascade import py_amcm
    return py_amcm(require_signed=require_signed)


@contextmanager
def _runtime_environment(args):
    mapping = {
        "input_hash": "CASCADE_INPUT_HASH_MODE",
        "output_hash": "CASCADE_PROVENANCE_HASH_MODE",
        "workers": "CASCADE_DAG_MAX_WORKERS",
        "timeout": "CASCADE_ISOLATED_TIMEOUT_SECONDS",
        "progress_interval_ms": "CASCADE_PROGRESS_INTERVAL_MS",
    }
    previous = {}
    try:
        for attribute, variable in mapping.items():
            value = getattr(args, attribute, None)
            if value is None:
                continue
            previous[variable] = os.environ.get(variable)
            os.environ[variable] = str(value)
        yield
    finally:
        for variable, value in previous.items():
            if value is None:
                os.environ.pop(variable, None)
            else:
                os.environ[variable] = value


@contextmanager
def _redirect_stdout_to_stderr(enabled: bool):
    if not enabled:
        yield
        return
    sys.stdout.flush()
    saved_stdout = os.dup(sys.stdout.fileno())
    try:
        os.dup2(sys.stderr.fileno(), sys.stdout.fileno())
        yield
    finally:
        sys.stdout.flush()
        os.dup2(saved_stdout, sys.stdout.fileno())
        os.close(saved_stdout)


def _load_mapping(path: str) -> Dict[str, Any]:
    if not os.path.isfile(path):
        raise FileNotFoundError(f"configuration file not found: {path}")
    with open(path, "r", encoding="utf-8") as source:
        if path.lower().endswith(".json"):
            data = json.load(source)
        else:
            try:
                import yaml
            except ImportError as error:
                raise RuntimeError("PyYAML is required to read YAML configuration") from error
            data = yaml.safe_load(source)
    if data is None:
        return {}
    if not isinstance(data, dict):
        raise TypeError(f"configuration root must be a mapping: {path}")
    return data


def _parameter_values(data: Dict[str, Any]) -> Dict[str, Any]:
    values = {}
    for key, value in data.items():
        if isinstance(value, dict) and "value" in value and ("type" in value or "description" in value):
            values[key] = value["value"]
        else:
            values[key] = value
    return values


def _apply_parameters(handle, values: Dict[str, Any]) -> None:
    for key, value in _parameter_values(values).items():
        handle.set_param(key, value)


def _result_payload(result, name: str) -> Dict[str, Any]:
    status = getattr(result.status, "name", str(result.status).rsplit(".", 1)[-1])
    phase = getattr(result.phase, "name", str(result.phase).rsplit(".", 1)[-1])
    if status.isupper():
        status = status.title()
    if phase == "None_":
        phase = "None"
    elif phase.isupper():
        phase = phase.title()
    return {
        "name": name,
        "status": status,
        "phase": phase,
        "message": str(getattr(result, "message", "")),
        "cache_decision": str(getattr(result, "cache_decision", "not_checked")),
        "cache_reason": str(getattr(result, "cache_reason", "")),
        "succeeded": bool(result.succeeded()),
        "allows_dependents": bool(result.allows_dependents()),
    }


def _resolve_config_path(base: str, value: Optional[str]) -> Optional[str]:
    if value is None:
        return None
    if not isinstance(value, str) or not value:
        raise TypeError("workflow paths must be non-empty strings")
    return os.path.realpath(value if os.path.isabs(value) else os.path.join(base, value))


def _validate_keys(value: Dict[str, Any], allowed: set, context: str) -> None:
    unknown = sorted(set(value) - allowed)
    if unknown:
        raise ValueError(f"{context} contains unknown fields: {', '.join(unknown)}")


def _split_parameter_ref(value: str, context: str) -> Tuple[str, str]:
    if not isinstance(value, str) or "." not in value:
        raise ValueError(f"{context} must use 'node.parameter' syntax")
    node, parameter = value.rsplit(".", 1)
    if not node or not parameter:
        raise ValueError(f"{context} must use 'node.parameter' syntax")
    return node, parameter
