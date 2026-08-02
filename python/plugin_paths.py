import json
import os
import tempfile
from contextlib import contextmanager


CONFIG_SCHEMA = 1


def config_path():
    configured = os.environ.get("CASCADE_CONFIG_FILE")
    if configured:
        return os.path.abspath(os.path.expanduser(configured))
    config_home = os.environ.get("XDG_CONFIG_HOME")
    if not config_home:
        config_home = os.path.join(os.path.expanduser("~"), ".config")
    return os.path.join(os.path.abspath(os.path.expanduser(config_home)), "cascade", "config.json")


def canonical_prefix(path):
    return os.path.realpath(os.path.abspath(os.path.expanduser(path)))


def _empty_config():
    return {"schema": CONFIG_SCHEMA, "plugin_prefixes": []}


def load_config(path=None):
    path = path or config_path()
    if not os.path.exists(path):
        return _empty_config()
    with open(path, "r", encoding="utf-8") as source:
        document = json.load(source)
    if not isinstance(document, dict):
        raise RuntimeError(f"Cascade config must be a JSON object: {path}")
    if document.get("schema") != CONFIG_SCHEMA:
        raise RuntimeError(f"Unsupported Cascade config schema in {path}")
    entries = document.get("plugin_prefixes", [])
    if not isinstance(entries, list):
        raise RuntimeError(f"Cascade config plugin_prefixes must be a list: {path}")

    normalized = []
    seen = set()
    for entry in entries:
        if isinstance(entry, str):
            raw_path = entry
            enabled = True
        elif isinstance(entry, dict):
            raw_path = entry.get("path")
            enabled = entry.get("enabled", True)
        else:
            raise RuntimeError(f"Invalid plugin prefix entry in {path}: {entry!r}")
        if not isinstance(raw_path, str) or not raw_path.strip():
            raise RuntimeError(f"Plugin prefix path must be a non-empty string in {path}")
        if not isinstance(enabled, bool):
            raise RuntimeError(f"Plugin prefix enabled flag must be boolean in {path}")
        prefix = canonical_prefix(raw_path)
        if prefix in seen:
            continue
        seen.add(prefix)
        normalized.append({"path": prefix, "enabled": enabled})
    return {"schema": CONFIG_SCHEMA, "plugin_prefixes": normalized}


def configured_plugin_prefixes(path=None):
    return [
        entry["path"]
        for entry in load_config(path).get("plugin_prefixes", [])
        if entry.get("enabled", True)
    ]


def _atomic_write_config(document, path):
    directory = os.path.dirname(path)
    os.makedirs(directory, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix="config.", suffix=".tmp", dir=directory)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as output:
            json.dump(document, output, indent=2, sort_keys=True)
            output.write("\n")
            output.flush()
            os.fsync(output.fileno())
        os.chmod(temporary, 0o600)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.remove(temporary)


@contextmanager
def _config_lock(path):
    directory = os.path.dirname(path)
    os.makedirs(directory, exist_ok=True)
    lock_path = path + ".lock"
    with open(lock_path, "a+", encoding="utf-8") as lock:
        try:
            import fcntl
        except ImportError:
            fcntl = None
        if fcntl is not None:
            fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
        try:
            yield
        finally:
            if fcntl is not None:
                fcntl.flock(lock.fileno(), fcntl.LOCK_UN)


def add_plugin_prefix(prefix, path=None):
    path = path or config_path()
    prefix = canonical_prefix(prefix)
    with _config_lock(path):
        document = load_config(path)
        entries = document["plugin_prefixes"]
        for entry in entries:
            if entry["path"] == prefix:
                changed = not entry.get("enabled", True)
                entry["enabled"] = True
                if changed:
                    _atomic_write_config(document, path)
                return changed
        entries.append({"path": prefix, "enabled": True})
        _atomic_write_config(document, path)
    return True


def remove_plugin_prefix(prefix, path=None):
    path = path or config_path()
    prefix = canonical_prefix(prefix)
    with _config_lock(path):
        document = load_config(path)
        original = list(document["plugin_prefixes"])
        document["plugin_prefixes"] = [entry for entry in original if entry["path"] != prefix]
        if len(document["plugin_prefixes"]) == len(original):
            return False
        _atomic_write_config(document, path)
    return True


def plugin_layout(prefix):
    prefix = canonical_prefix(prefix)
    return {
        "prefix": prefix,
        "cpp": os.path.join(prefix, "lib", "cascade", "plugin"),
        "python": os.path.join(prefix, "lib", "cascade", "pyplugin"),
        "include": os.path.join(prefix, "include", "cascade", "plugin"),
        "trust_store": os.path.join(prefix, "share", "cascade", "trusted_keys"),
    }


def unique_paths(paths):
    result = []
    seen = set()
    for path in paths:
        canonical = os.path.realpath(os.path.abspath(os.path.expanduser(path)))
        if canonical in seen:
            continue
        seen.add(canonical)
        result.append(canonical)
    return result
