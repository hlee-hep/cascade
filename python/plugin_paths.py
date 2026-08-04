import json
import os
import stat
import tempfile
from contextlib import contextmanager


CONFIG_SCHEMA = 1


def config_path():
    configured = os.environ.get("CASCADE_CONFIG_FILE")
    if configured:
        return os.path.abspath(os.path.expanduser(configured))
    config_home = os.environ.get("XDG_CONFIG_HOME")
    if not config_home:
        home = os.environ.get("HOME") or os.environ.get("USERPROFILE") or os.path.expanduser("~")
        config_home = os.path.join(home, ".config")
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
    _ensure_real_directory(directory)
    descriptor, temporary = tempfile.mkstemp(prefix="config.", suffix=".tmp", dir=directory)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as output:
            json.dump(document, output, indent=2, sort_keys=True)
            output.write("\n")
            output.flush()
            os.fsync(output.fileno())
        os.chmod(temporary, 0o600)
        if os.rename in os.supports_dir_fd:
            directory_descriptor = _open_real_directory(directory)
            try:
                os.replace(
                    os.path.basename(temporary),
                    os.path.basename(path),
                    src_dir_fd=directory_descriptor,
                    dst_dir_fd=directory_descriptor,
                )
                os.fsync(directory_descriptor)
            finally:
                os.close(directory_descriptor)
        else:
            os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.remove(temporary)


@contextmanager
def _config_lock(path):
    directory = os.path.dirname(path)
    _ensure_real_directory(directory)
    lock_path = path + ".lock"
    flags = os.O_RDWR | os.O_CREAT
    if hasattr(os, "O_BINARY"):
        flags |= os.O_BINARY
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    if os.open in os.supports_dir_fd:
        directory_descriptor = _open_real_directory(directory)
        try:
            descriptor = os.open(os.path.basename(lock_path), flags, 0o600, dir_fd=directory_descriptor)
        finally:
            os.close(directory_descriptor)
    else:
        descriptor = os.open(lock_path, flags, 0o600)
    if not stat.S_ISREG(os.fstat(descriptor).st_mode):
        os.close(descriptor)
        raise RuntimeError(f"Cascade config lock must be a regular file: {lock_path}")
    with os.fdopen(descriptor, "a+b") as lock:
        if os.name == "nt":
            import msvcrt
            lock.seek(0)
            if os.path.getsize(lock_path) == 0:
                lock.write(b"\0")
                lock.flush()
            lock.seek(0)
            msvcrt.locking(lock.fileno(), msvcrt.LK_LOCK, 1)
            try:
                yield
            finally:
                lock.seek(0)
                msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)
        else:
            import fcntl
            fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
            try:
                yield
            finally:
                fcntl.flock(lock.fileno(), fcntl.LOCK_UN)


def _ensure_real_directory(directory):
    directory = os.path.abspath(directory)
    missing = []
    current = directory
    while not os.path.lexists(current):
        missing.append(current)
        parent = os.path.dirname(current)
        if parent == current:
            break
        current = parent
    while True:
        metadata = os.lstat(current)
        if stat.S_ISLNK(metadata.st_mode):
            raise RuntimeError(f"Cascade config parent must not be a symbolic link: {current}")
        if not stat.S_ISDIR(metadata.st_mode):
            raise RuntimeError(f"Cascade config parent is not a directory: {current}")
        parent = os.path.dirname(current)
        if parent == current:
            break
        current = parent
    for path in reversed(missing):
        try:
            os.mkdir(path, 0o700)
        except FileExistsError:
            metadata = os.lstat(path)
            if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISDIR(metadata.st_mode):
                raise RuntimeError(f"Cascade config parent is not a real directory: {path}")


def _open_real_directory(directory):
    flags = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(os.sep, flags)
    try:
        for component in os.path.abspath(directory).split(os.sep):
            if not component:
                continue
            next_descriptor = os.open(component, flags, dir_fd=descriptor)
            os.close(descriptor)
            descriptor = next_descriptor
        result = descriptor
        descriptor = -1
        return result
    finally:
        if descriptor >= 0:
            os.close(descriptor)


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
