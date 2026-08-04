import ctypes
import contextlib
import hashlib
import importlib.util
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
from typing import Any, Dict, List, Optional, Tuple

from .common import _CLI_PREFIX, _emit, _log

try:
    from cascade.plugin_paths import (
        add_plugin_prefix,
        canonical_prefix,
        config_path,
        configured_plugin_prefixes,
        load_config,
        plugin_layout,
        remove_plugin_prefix,
        unique_paths,
    )
except ImportError:
    _PLUGIN_PATHS_ROOT = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
    _PLUGIN_PATHS_CANDIDATES = (
        os.path.join(_PLUGIN_PATHS_ROOT, "cascade", "plugin_paths.py"),
        os.path.join(_PLUGIN_PATHS_ROOT, "plugin_paths.py"),
    )
    _PLUGIN_PATHS_FILE = next(
        (path for path in _PLUGIN_PATHS_CANDIDATES if os.path.isfile(path)),
        _PLUGIN_PATHS_CANDIDATES[-1],
    )
    _PLUGIN_PATHS_SPEC = importlib.util.spec_from_file_location("cascade_cli_plugin_paths", _PLUGIN_PATHS_FILE)
    _PLUGIN_PATHS_MODULE = importlib.util.module_from_spec(_PLUGIN_PATHS_SPEC)
    _PLUGIN_PATHS_SPEC.loader.exec_module(_PLUGIN_PATHS_MODULE)
    add_plugin_prefix = _PLUGIN_PATHS_MODULE.add_plugin_prefix
    canonical_prefix = _PLUGIN_PATHS_MODULE.canonical_prefix
    config_path = _PLUGIN_PATHS_MODULE.config_path
    configured_plugin_prefixes = _PLUGIN_PATHS_MODULE.configured_plugin_prefixes
    load_config = _PLUGIN_PATHS_MODULE.load_config
    plugin_layout = _PLUGIN_PATHS_MODULE.plugin_layout
    remove_plugin_prefix = _PLUGIN_PATHS_MODULE.remove_plugin_prefix
    unique_paths = _PLUGIN_PATHS_MODULE.unique_paths


def _sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def _verify_manifest_signature(manifest: str, pubkey: str) -> bool:
    sig = manifest + ".sig"
    try:
        result = subprocess.run(
            ["openssl", "pkeyutl", "-verify", "-pubin", "-inkey", pubkey, "-rawin", "-in", manifest, "-sigfile", sig],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    except OSError:
        return False
    return result.returncode == 0


def _read_regular_file(path: str) -> bytes:
    flags = os.O_RDONLY
    if hasattr(os, "O_BINARY"):
        flags |= os.O_BINARY
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    descriptor = os.open(path, flags)
    try:
        metadata = os.fstat(descriptor)
        if not stat.S_ISREG(metadata.st_mode):
            raise RuntimeError(f"Expected a regular file: {path}")
        chunks = []
        while True:
            chunk = os.read(descriptor, 1024 * 1024)
            if not chunk:
                break
            chunks.append(chunk)
        return b"".join(chunks)
    finally:
        os.close(descriptor)


def _snapshot_trusted_keys(target_prefix: str, public_key: Optional[str]) -> List[Dict[str, Any]]:
    snapshots = []
    trust_store = plugin_layout(target_prefix)["trust_store"]
    if os.path.isdir(trust_store) and not os.path.islink(trust_store):
        for name in sorted(os.listdir(trust_store)):
            path = os.path.join(trust_store, name)
            if not name.endswith(".pem"):
                continue
            try:
                data = _read_regular_file(path)
            except (OSError, RuntimeError):
                continue
            snapshots.append({
                "name": name,
                "data": data,
                "fingerprint": hashlib.sha256(data).hexdigest(),
                "operator_supplied": False,
            })
    if public_key:
        path = os.path.realpath(os.path.abspath(os.path.expanduser(public_key)))
        data = _read_regular_file(path)
        fingerprint = hashlib.sha256(data).hexdigest()
        snapshots = [entry for entry in snapshots if entry["fingerprint"] != fingerprint]
        snapshots.append({
            "name": os.path.basename(path),
            "data": data,
            "fingerprint": fingerprint,
            "operator_supplied": True,
        })
    return snapshots


@contextlib.contextmanager
def _materialized_trusted_keys(snapshots: List[Dict[str, Any]]):
    with tempfile.TemporaryDirectory(prefix="cascade-trusted-keys-") as directory:
        paths = []
        snapshot_by_path = {}
        for index, snapshot in enumerate(snapshots):
            path = os.path.join(directory, f"{index:04d}.pem")
            descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(descriptor, "wb") as output:
                output.write(snapshot["data"])
            paths.append(path)
            snapshot_by_path[path] = snapshot
        yield paths, snapshot_by_path


def _ensure_real_directory_tree(path: str, allow_missing_leaf: bool = False) -> None:
    absolute = os.path.abspath(path)
    parts = absolute.split(os.sep)
    current = os.sep if absolute.startswith(os.sep) else parts.pop(0)
    for index, part in enumerate(parts):
        if not part:
            continue
        current = os.path.join(current, part)
        try:
            metadata = os.lstat(current)
        except FileNotFoundError:
            if allow_missing_leaf:
                return
            raise
        if stat.S_ISLNK(metadata.st_mode):
            raise RuntimeError(f"Symbolic links are not allowed in plugin paths: {current}")
        if not stat.S_ISDIR(metadata.st_mode):
            raise RuntimeError(f"Plugin path component is not a directory: {current}")


def _validate_staged_tree(package_dir: str) -> None:
    metadata = os.lstat(package_dir)
    if not stat.S_ISDIR(metadata.st_mode) or stat.S_ISLNK(metadata.st_mode):
        raise RuntimeError(f"Staged plugin package must be a real directory: {package_dir}")
    for root, directories, files in os.walk(package_dir, followlinks=False):
        for name in directories + files:
            path = os.path.join(root, name)
            item = os.lstat(path)
            if stat.S_ISLNK(item.st_mode):
                raise RuntimeError(f"Staged plugin contains a symbolic link: {path}")
            if name in files and not stat.S_ISREG(item.st_mode):
                raise RuntimeError(f"Staged plugin contains a non-regular file: {path}")


@contextlib.contextmanager
def _single_lock(lock_path: str, exclusive: bool):
    lock_directory = os.path.dirname(lock_path)
    os.makedirs(lock_directory, exist_ok=True)
    flags = os.O_RDWR | os.O_CREAT
    if hasattr(os, "O_BINARY"):
        flags |= os.O_BINARY
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    descriptor = os.open(lock_path, flags, 0o600)
    if not stat.S_ISREG(os.fstat(descriptor).st_mode):
        os.close(descriptor)
        raise RuntimeError(f"Plugin lock is not a regular file: {lock_path}")
    with os.fdopen(descriptor, "a+b") as lock:
        if os.name == "nt":
            import msvcrt
            lock.seek(0)
            if os.path.getsize(lock_path) == 0:
                lock.write(b"\0")
                lock.flush()
            lock.seek(0)
            mode = msvcrt.LK_LOCK if exclusive else msvcrt.LK_RLCK
            msvcrt.locking(lock.fileno(), mode, 1)
            try:
                yield
            finally:
                lock.seek(0)
                msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)
        else:
            import fcntl
            mode = fcntl.LOCK_EX if exclusive else fcntl.LOCK_SH
            fcntl.flock(lock.fileno(), mode)
            try:
                yield
            finally:
                fcntl.flock(lock.fileno(), fcntl.LOCK_UN)


@contextlib.contextmanager
def _package_lock(prefix: str, package: str, exclusive: bool):
    lock_directory = os.path.join(prefix, ".cascade-locks")
    _ensure_real_directory_tree(prefix)
    if os.path.lexists(lock_directory):
        _ensure_real_directory_tree(lock_directory)
    else:
        os.mkdir(lock_directory, 0o700)
    with _single_lock(os.path.join(lock_directory, ".index.lock"), exclusive):
        with _single_lock(os.path.join(lock_directory, package + ".lock"), exclusive):
            try:
                yield
            finally:
                pass


def _default_plugin_dirs() -> Tuple[str, str]:
    cpp_dir = os.environ.get("CASCADE_PLUGIN_DIR", os.path.join(_CLI_PREFIX, "lib", "cascade", "plugin"))
    py_dir = os.environ.get("CASCADE_PYPLUGIN_DIR", os.path.join(_CLI_PREFIX, "lib", "cascade", "pyplugin"))
    return cpp_dir, py_dir


def _runtime_plugin_layouts() -> List[Dict[str, str]]:
    layouts = []
    trust_override = os.environ.get("CASCADE_PLUGIN_TRUST_STORE")
    cpp_override = os.environ.get("CASCADE_PLUGIN_DIR")
    python_override = os.environ.get("CASCADE_PYPLUGIN_DIR")
    if cpp_override or python_override:
        layouts.append({
            "prefix": "environment",
            "cpp": os.path.realpath(cpp_override) if cpp_override else "",
            "python": os.path.realpath(python_override) if python_override else "",
            "trust_store": os.path.realpath(
                trust_override or os.path.join(_CLI_PREFIX, "share", "cascade", "trusted_keys")
            ),
            "source": "environment",
        })
    for prefix in configured_plugin_prefixes():
        layout = plugin_layout(prefix)
        layout["source"] = "config"
        if trust_override:
            layout["trust_store"] = os.path.realpath(trust_override)
        layouts.append(layout)
    default_layout = plugin_layout(_CLI_PREFIX)
    default_layout["source"] = "cascade"
    if trust_override:
        default_layout["trust_store"] = os.path.realpath(trust_override)
    layouts.append(default_layout)

    result = []
    seen = set()
    for layout in layouts:
        key = (layout["cpp"], layout["python"], layout["trust_store"])
        if key in seen:
            continue
        seen.add(key)
        result.append(layout)
    return result


def _manifest_classes(path: str) -> List[str]:
    try:
        import ast
        with open(path, "r", encoding="utf-8") as f:
            tree = ast.parse(f.read(), filename=path)
    except Exception:
        return []

    def is_base_module(base) -> bool:
        if isinstance(base, ast.Name):
            return base.id == "base_module"
        if isinstance(base, ast.Attribute):
            return base.attr == "base_module"
        return False

    classes = []
    for node in tree.body:
        if isinstance(node, ast.ClassDef) and any(is_base_module(base) for base in node.bases):
            classes.append(node.name)
    return classes


def _check_cpp_abi(path: str) -> Tuple[str, Optional[str]]:
    try:
        import cascade
        expected = int(getattr(cascade, "__abi_version__"))
        expected_tag = str(getattr(cascade, "__abi_tag__"))
    except Exception as e:
        return "WARN", f"cannot import cascade ABI version: {e}"

    try:
        lib = ctypes.CDLL(path)
    except OSError as e:
        return "ERROR", f"dlopen failed: {e}"

    try:
        fn = lib.CascadePluginAbiVersion
    except AttributeError:
        return "ERROR", "missing CascadePluginAbiVersion"
    fn.restype = ctypes.c_int
    try:
        actual = int(fn())
    except Exception as e:
        return "ERROR", f"CascadePluginAbiVersion failed: {e}"
    if actual != expected:
        return "ERROR", f"ABI mismatch: plugin={actual} cascade={expected}"
    try:
        tag_fn = lib.CascadePluginAbiTag
    except AttributeError:
        return "ERROR", "missing CascadePluginAbiTag"
    tag_fn.restype = ctypes.c_char_p
    try:
        raw_tag = tag_fn()
        actual_tag = raw_tag.decode("utf-8") if raw_tag else ""
    except Exception as e:
        return "ERROR", f"CascadePluginAbiTag failed: {e}"
    if actual_tag != expected_tag:
        return "ERROR", f"ABI tag mismatch: plugin={actual_tag!r} cascade={expected_tag!r}"
    return "OK", f"ABI {actual}, tag {actual_tag}"


def _doctor_plugin_package(
    path: str,
    language: str,
    check_abi: bool,
    trusted_keys: List[str],
    require_signed: bool = False,
    reports: Optional[List[Dict[str, Any]]] = None,
    quiet: bool = False,
) -> Tuple[int, List[str]]:
    errors = 0
    warnings = 0
    names = []
    trust = "VERIFIED"
    verified_key = None

    def emit(level: str, message: str) -> None:
        if not quiet:
            _log(level, message)

    def finish() -> Tuple[int, List[str]]:
        if reports is not None:
            reports.append({
                "package": os.path.basename(os.path.abspath(path)),
                "language": language,
                "trust": trust,
                "modules": list(names),
                "errors": errors,
                "warnings": warnings,
                "path": os.path.realpath(path),
                "verified_key": verified_key,
            })
        return errors, names

    emit("INFO", f"[{language}] directory: {path}")
    if not os.path.isdir(path):
        emit("WARN", f"[{language}] directory not found")
        warnings += 1
        return finish()

    manifest = os.path.join(path, "plugin_manifest.json")
    manifest_sig = manifest + ".sig"
    if not os.path.isfile(manifest):
        emit("ERROR", f"[{language}] missing plugin_manifest.json")
        errors += 1
        return finish()

    has_signature = os.path.isfile(manifest_sig)
    verified_key = next(
        (key for key in trusted_keys if has_signature and _verify_manifest_signature(manifest, key)),
        None,
    )
    if verified_key:
        trust = "SIGNED"
        emit("INFO", f"[{language}] manifest signature: OK ({os.path.basename(verified_key)})")
    elif require_signed:
        emit("ERROR", f"[{language}] package requires a trusted manifest signature")
        errors += 1
        return finish()
    elif has_signature:
        emit("WARN", f"[{language}] manifest signature is not trusted; package is only verified")
        warnings += 1
    else:
        emit("INFO", f"[{language}] unsigned manifest: verified policy")

    try:
        with open(manifest, "r", encoding="utf-8") as source:
            data = json.load(source)
    except Exception as error:
        emit("ERROR", f"[{language}] cannot parse plugin_manifest.json: {error}")
        errors += 1
        return finish()

    if data.get("schema") != 2:
        emit("ERROR", f"[{language}] unsupported manifest schema")
        errors += 1
        return finish()
    if data.get("package") != os.path.basename(os.path.abspath(path)):
        emit("ERROR", f"[{language}] manifest package does not match directory name")
        errors += 1
        return finish()

    listed = set()
    modules = data.get("modules", [])
    if not isinstance(modules, list):
        emit("ERROR", f"[{language}] manifest field 'modules' must be a list")
        errors += 1
        return finish()

    for entry in modules:
        if not isinstance(entry, dict):
            emit("ERROR", f"[{language}] invalid manifest module entry: {entry!r}")
            errors += 1
            continue
        if entry.get("language") != language:
            continue
        relative = entry.get("path", "")
        name = entry.get("name", relative)
        identities = entry.get("classes", []) if language == "python" else [name]
        if not isinstance(identities, list) or not all(isinstance(identity, str) and identity for identity in identities):
            emit("ERROR", f"[{language}] {name}: module identities must be non-empty strings")
            errors += 1
            identities = []
        for identity in identities:
            if identity in names:
                emit("ERROR", f"[{language}] duplicate module identity in manifest: {identity}")
                errors += 1
            names.append(identity)
        if not relative or os.path.isabs(relative):
            emit("ERROR", f"[{language}] {name}: invalid relative path {relative!r}")
            errors += 1
            continue
        listed.add(relative)
        full = os.path.realpath(os.path.join(path, relative))
        if os.path.commonpath([os.path.realpath(path), full]) != os.path.realpath(path):
            emit("ERROR", f"[{language}] {name}: path escapes package directory")
            errors += 1
            continue
        if not os.path.isfile(full):
            emit("ERROR", f"[{language}] {name}: listed file missing: {relative}")
            errors += 1
            continue
        expected = entry.get("sha256", "")
        actual = _sha256_file(full)
        if actual != expected:
            emit("ERROR", f"[{language}] {name}: hash mismatch")
            errors += 1
            continue
        if language == "cpp":
            if not os.path.basename(full).endswith("Module.so"):
                emit("WARN", f"[{language}] {name}: filename does not end with Module.so")
                warnings += 1
            elif check_abi:
                level, message = _check_cpp_abi(full)
                emit(level, f"[{language}] {name}: {message}")
                if level == "ERROR":
                    errors += 1
                elif level == "WARN":
                    warnings += 1
            else:
                emit("INFO", f"[{language}] {name}: hash OK")
        elif language == "python":
            if os.path.basename(full) != "__init__.py" and not os.path.basename(full).endswith("module.py"):
                emit("WARN", f"[{language}] {name}: filename does not end with module.py")
                warnings += 1
            declared = entry.get("classes", [])
            discovered = _manifest_classes(full)
            missing = [class_name for class_name in declared if class_name not in discovered]
            if missing:
                emit("ERROR", f"[{language}] {name}: classes missing from source: {missing}")
                errors += 1
            else:
                emit("INFO", f"[{language}] {name}: hash OK, classes={declared or discovered}")

    expected_suffix = "Module.so" if language == "cpp" else "module.py"
    ignored = []
    for filename in sorted(os.listdir(path)):
        if filename in {"plugin_manifest.json", "plugin_manifest.json.sig", "__init__.py"}:
            continue
        if filename.endswith(expected_suffix) and filename not in listed:
            ignored.append(filename)
    if ignored:
        emit("WARN", f"[{language}] files ignored because they are not in manifest: {', '.join(ignored)}")
        warnings += 1

    emit(trust, f"[{language}] {os.path.basename(path)} modules={len(listed)} errors={errors} warnings={warnings}")
    return finish()


def _doctor_plugin_dir(
    path: str,
    language: str,
    check_abi: bool,
    trusted_keys: List[str],
    require_signed: bool = False,
    reports: Optional[List[Dict[str, Any]]] = None,
    quiet: bool = False,
) -> Tuple[int, List[str]]:
    if not quiet:
        _log("INFO", f"[{language}] package root: {path}")
    if not os.path.isdir(path):
        if not quiet:
            _log("WARN", f"[{language}] package root not found")
        return 0, []
    errors = 0
    names = []
    package_dirs = [
        os.path.join(path, entry)
        for entry in sorted(os.listdir(path))
        if os.path.isdir(os.path.join(path, entry))
    ]
    for package_dir in package_dirs:
        package_errors, package_names = _doctor_plugin_package(
            package_dir,
            language,
            check_abi,
            trusted_keys,
            require_signed=require_signed,
            reports=reports,
            quiet=quiet,
        )
        errors += package_errors
        names.extend(package_names)
    return errors, names


def cmd_doctor_plugins(args) -> None:
    require_signed = bool(getattr(args, "require_signed", False))
    quiet = bool(getattr(args, "json", False))
    reports = []
    if args.cpp_dir or args.py_dir:
        cpp_default, py_default = _default_plugin_dirs()
        layouts = [{
            "prefix": "explicit",
            "cpp": os.path.realpath(args.cpp_dir or cpp_default),
            "python": os.path.realpath(args.py_dir or py_default),
            "trust_store": os.path.realpath(
                args.trust_store
                or os.environ.get("CASCADE_PLUGIN_TRUST_STORE")
                or os.path.join(_CLI_PREFIX, "share", "cascade", "trusted_keys")
            ),
            "source": "explicit",
        }]
    else:
        layouts = _runtime_plugin_layouts()
        if args.trust_store:
            for layout in layouts:
                layout["trust_store"] = os.path.realpath(args.trust_store)

    errors = 0
    cpp_names = []
    py_names = []
    for layout in layouts:
        trust_store = layout["trust_store"]
        trusted_keys = []
        if os.path.isdir(trust_store):
            trusted_keys = [
                os.path.join(trust_store, entry)
                for entry in sorted(os.listdir(trust_store))
                if entry.endswith(".pem") and os.path.isfile(os.path.join(trust_store, entry))
            ]
        if require_signed and not trusted_keys and not quiet:
            _log("WARN", f"no trusted plugin keys found in {trust_store}")

        report_start = len(reports)
        cpp_errors, layout_cpp_names = (0, [])
        py_errors, layout_py_names = (0, [])
        if layout["cpp"]:
            cpp_errors, layout_cpp_names = _doctor_plugin_dir(
                layout["cpp"], "cpp", not args.no_abi, trusted_keys, require_signed, reports, quiet
            )
        if layout["python"]:
            py_errors, layout_py_names = _doctor_plugin_dir(
                layout["python"], "python", False, trusted_keys, require_signed, reports, quiet
            )
        for report in reports[report_start:]:
            report["prefix"] = layout["prefix"]
            report["source"] = layout["source"]
        errors += cpp_errors + py_errors
        cpp_names.extend(layout_cpp_names)
        py_names.extend(layout_py_names)

    duplicate_cpp = sorted({name for name in cpp_names if cpp_names.count(name) > 1})
    duplicate_python = sorted({name for name in py_names if py_names.count(name) > 1})
    cross_language_duplicates = sorted(set(cpp_names) & set(py_names))
    duplicates = {
        "cpp": duplicate_cpp,
        "python": duplicate_python,
        "cross_language": cross_language_duplicates,
    }
    if any(duplicates.values()):
        if not quiet:
            _log("ERROR", f"duplicate plugin module names: {duplicates}")
        errors += 1
    if quiet:
        _emit({
            "require_signed": require_signed,
            "config": config_path(),
            "layouts": layouts,
            "packages": reports,
            "duplicates": duplicates,
            "succeeded": errors == 0,
        }, True)
    if errors:
        raise SystemExit(1)


def cmd_plugin_path_list(args) -> None:
    document = load_config()
    entries = []
    for entry in document["plugin_prefixes"]:
        layout = plugin_layout(entry["path"])
        entries.append({
            "path": entry["path"],
            "enabled": entry.get("enabled", True),
            "exists": os.path.isdir(entry["path"]),
            "cpp": layout["cpp"],
            "python": layout["python"],
            "trust_store": layout["trust_store"],
        })
    if args.json:
        _emit({"config": config_path(), "plugin_prefixes": entries}, True)
        return
    print(f"Config: {config_path()}")
    if not entries:
        print("No persistent plugin prefixes configured.")
        return
    for entry in entries:
        state = "enabled" if entry["enabled"] else "disabled"
        availability = "present" if entry["exists"] else "missing"
        print(f"{entry['path']} ({state}, {availability})")


def cmd_plugin_path_add(args) -> None:
    prefix = canonical_prefix(args.prefix)
    if not os.path.isdir(prefix):
        if not args.create:
            raise FileNotFoundError(f"Plugin prefix does not exist: {prefix}")
        os.makedirs(prefix, exist_ok=True)
    changed = add_plugin_prefix(prefix)
    _emit({
        "prefix": prefix,
        "config": config_path(),
        "changed": changed,
    }, args.json)


def cmd_plugin_path_remove(args) -> None:
    prefix = canonical_prefix(args.prefix)
    changed = remove_plugin_prefix(prefix)
    if not changed:
        raise RuntimeError(f"Plugin prefix is not registered: {prefix}")
    _emit({
        "prefix": prefix,
        "config": config_path(),
        "changed": True,
    }, args.json)


def _remove_path(path: str) -> None:
    if os.path.isdir(path) and not os.path.islink(path):
        shutil.rmtree(path)
    elif os.path.lexists(path):
        os.remove(path)


def _replace_path(source: str, destination: str) -> None:
    if os.rename not in os.supports_dir_fd:
        os.replace(source, destination)
        return
    source_parent = os.path.dirname(source)
    destination_parent = os.path.dirname(destination)
    source_descriptor = _open_directory_nofollow(source_parent)
    try:
        destination_descriptor = _open_directory_nofollow(destination_parent)
        try:
            os.replace(
                os.path.basename(source),
                os.path.basename(destination),
                src_dir_fd=source_descriptor,
                dst_dir_fd=destination_descriptor,
            )
        finally:
            os.close(destination_descriptor)
    finally:
        os.close(source_descriptor)


def _open_directory_nofollow(path: str) -> int:
    directory_flags = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
    if hasattr(os, "O_NOFOLLOW"):
        directory_flags |= os.O_NOFOLLOW
    absolute = os.path.abspath(path)
    descriptor = os.open(os.sep, directory_flags)
    try:
        for component in absolute.split(os.sep):
            if not component:
                continue
            next_descriptor = os.open(component, directory_flags, dir_fd=descriptor)
            os.close(descriptor)
            descriptor = next_descriptor
        result = descriptor
        descriptor = -1
        return result
    finally:
        if descriptor >= 0:
            os.close(descriptor)


def _publish_staged_plugin(
    stage_prefix: str,
    target_prefix: str,
    package: str,
    public_key_data: Optional[bytes] = None,
) -> List[Dict[str, Any]]:
    stage = plugin_layout(stage_prefix)
    target = plugin_layout(target_prefix)
    rollback_root = os.path.join(stage_prefix, ".rollback")
    os.makedirs(rollback_root, exist_ok=True)
    operations = []
    specifications = [
        (os.path.join(stage["cpp"], package), os.path.join(target["cpp"], package), "cpp"),
        (os.path.join(stage["python"], package), os.path.join(target["python"], package), "python"),
        (os.path.join(stage["include"], package), os.path.join(target["include"], package), "include"),
    ]
    staged_key = os.path.join(stage_prefix, ".operator-approved-key.pem")
    if public_key_data is not None:
        descriptor = os.open(staged_key, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, "wb") as output:
            output.write(public_key_data)
        specifications.append(
            (staged_key, os.path.join(target["trust_store"], package + ".pem"), "public-key")
        )

    try:
        for source, destination, label in specifications:
            if os.path.lexists(source):
                if label == "public-key":
                    _read_regular_file(source)
                else:
                    _validate_staged_tree(source)
            _ensure_real_directory_tree(os.path.dirname(destination), allow_missing_leaf=True)
            backup = os.path.join(rollback_root, label)
            operation = {
                "label": label,
                "destination": destination,
                "backup": backup,
                "had_destination": False,
                "published": False,
            }
            operations.append(operation)
            os.makedirs(os.path.dirname(destination), exist_ok=True)
            _ensure_real_directory_tree(os.path.dirname(destination))
            operation["had_destination"] = os.path.lexists(destination)
            if operation["had_destination"]:
                if os.path.islink(destination):
                    raise RuntimeError(f"Refusing to replace symlinked plugin destination: {destination}")
                _replace_path(destination, backup)
            if os.path.lexists(source):
                _replace_path(source, destination)
                operation["published"] = True
    except Exception as publish_error:
        try:
            _restore_publish_operations(operations)
        except Exception as rollback_error:
            raise RuntimeError(
                f"Plugin publication failed ({publish_error}); rollback was incomplete: {rollback_error}; "
                f"recovery data preserved at {rollback_root}"
            ) from publish_error
        raise
    return operations


def _restore_publish_operations(operations: List[Dict[str, Any]]) -> None:
    errors = []
    for operation in reversed(operations):
        destination = operation["destination"]
        backup = operation["backup"]
        try:
            if operation["published"] and os.path.lexists(destination):
                _remove_path(destination)
        except Exception as error:
            errors.append(f"{operation['label']} remove {destination}: {error}")
        try:
            if operation["had_destination"]:
                if os.path.lexists(destination):
                    errors.append(f"{operation['label']} destination still occupied: {destination}")
                elif os.path.lexists(backup):
                    os.makedirs(os.path.dirname(destination), exist_ok=True)
                    _replace_path(backup, destination)
                else:
                    errors.append(f"{operation['label']} backup missing: {backup}")
        except Exception as error:
            errors.append(f"{operation['label']} restore {backup} -> {destination}: {error}")
    for operation in operations:
        if operation["had_destination"] and not os.path.lexists(operation["destination"]):
            errors.append(f"{operation['label']} previous destination was not restored: {operation['destination']}")
    if errors:
        raise RuntimeError("; ".join(errors))


def _verify_staged_plugin(
    stage_prefix: str,
    package: str,
    require_signed: bool,
    trusted_key_snapshots: Optional[List[Dict[str, Any]]] = None,
    quiet: bool = False,
) -> Dict[str, Any]:
    layout = plugin_layout(stage_prefix)
    trusted_key_snapshots = trusted_key_snapshots or []
    reports = []
    errors = 0
    found = False
    with _materialized_trusted_keys(trusted_key_snapshots) as (trusted_keys, snapshot_by_path):
        for language, root, check_abi in (
            ("cpp", layout["cpp"], True),
            ("python", layout["python"], False),
        ):
            package_dir = os.path.join(root, package)
            if not os.path.lexists(package_dir):
                continue
            _validate_staged_tree(package_dir)
            found = True
            report_start = len(reports)
            package_errors, _ = _doctor_plugin_package(
                package_dir,
                language,
                check_abi,
                trusted_keys,
                require_signed=require_signed,
                reports=reports,
                quiet=quiet,
            )
            for report in reports[report_start:]:
                verified_path = report.pop("verified_key", None)
                snapshot = snapshot_by_path.get(verified_path)
                report["signer_fingerprint"] = snapshot["fingerprint"] if snapshot else None
                report["operator_signed"] = bool(snapshot and snapshot["operator_supplied"])
            errors += package_errors
    if not found:
        raise RuntimeError("Plugin build did not install a C++ or Python package")
    if errors:
        raise RuntimeError(f"Staged plugin verification failed with {errors} error(s)")
    return {"packages": reports, "trust_store": layout["trust_store"]}


def _other_prefix_package_locations(package: str, target_prefix: str) -> List[str]:
    target_prefix = canonical_prefix(target_prefix)
    prefixes = unique_paths(configured_plugin_prefixes() + [os.path.realpath(_CLI_PREFIX)])
    locations = []
    for prefix in prefixes:
        if prefix == target_prefix:
            continue
        layout = plugin_layout(prefix)
        for root in (layout["cpp"], layout["python"]):
            package_dir = os.path.join(root, package)
            if os.path.isdir(package_dir):
                locations.append(package_dir)
    for root in (os.environ.get("CASCADE_PLUGIN_DIR"), os.environ.get("CASCADE_PYPLUGIN_DIR")):
        if root:
            package_dir = os.path.join(os.path.realpath(root), package)
            if os.path.isdir(package_dir) and package_dir not in locations:
                locations.append(package_dir)
    return locations


def _manifest_module_identities(package_dir: str) -> List[Tuple[str, str]]:
    manifest_path = os.path.join(package_dir, "plugin_manifest.json")
    try:
        document = json.loads(_read_regular_file(manifest_path).decode("utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError, RuntimeError):
        return []
    identities = []
    for entry in document.get("modules", []):
        if not isinstance(entry, dict):
            continue
        language = entry.get("language")
        if language == "python":
            names = entry.get("classes", [])
        elif language == "cpp":
            names = [entry.get("name")]
        else:
            continue
        for name in names:
            if isinstance(name, str) and name:
                identities.append((language, name))
    return identities


def _reject_prospective_duplicates(stage_prefix: str, target_prefix: str, package: str) -> None:
    staged_layout = plugin_layout(stage_prefix)
    target_layout = plugin_layout(target_prefix)
    candidates = []
    for language, root in (("cpp", staged_layout["cpp"]), ("python", staged_layout["python"])):
        package_dir = os.path.join(root, package)
        for declared_language, name in _manifest_module_identities(package_dir):
            candidates.append((declared_language, name, package_dir))

    replaced = {
        os.path.realpath(os.path.join(target_layout["cpp"], package)),
        os.path.realpath(os.path.join(target_layout["python"], package)),
    }
    for layout in _runtime_plugin_layouts():
        for language, root in (("cpp", layout["cpp"]), ("python", layout["python"])):
            if not root or not os.path.isdir(root) or os.path.islink(root):
                continue
            for entry in sorted(os.listdir(root)):
                package_dir = os.path.join(root, entry)
                if os.path.realpath(package_dir) in replaced:
                    continue
                try:
                    metadata = os.lstat(package_dir)
                except OSError:
                    continue
                if not stat.S_ISDIR(metadata.st_mode) or stat.S_ISLNK(metadata.st_mode):
                    continue
                for declared_language, name in _manifest_module_identities(package_dir):
                    candidates.append((declared_language, name, package_dir))

    by_name = {}
    for language, name, package_dir in candidates:
        by_name.setdefault(name, []).append((language, package_dir))
    duplicates = {name: locations for name, locations in by_name.items() if len(locations) > 1}
    if duplicates:
        details = []
        for name, locations in sorted(duplicates.items()):
            origins = ", ".join(f"{language}:{path}" for language, path in locations)
            details.append(f"{name} ({origins})")
        raise RuntimeError("Duplicate plugin module names in prospective installation: " + "; ".join(details))


def cmd_plugin_install(args) -> None:
    source = os.path.realpath(os.path.abspath(os.path.expanduser(args.source)))
    if not os.path.isdir(source):
        raise FileNotFoundError(f"Plugin source directory not found: {source}")
    if not os.path.isfile(os.path.join(source, "SConstruct")):
        raise FileNotFoundError(f"Plugin source does not contain SConstruct: {source}")
    package = args.package or os.path.basename(source)
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", package):
        raise ValueError("Plugin package must contain only letters, digits, '.', '_', or '-'")
    target_prefix = canonical_prefix(args.prefix)
    os.makedirs(target_prefix, exist_ok=True)
    _ensure_real_directory_tree(target_prefix)
    trusted_key_snapshots = _snapshot_trusted_keys(target_prefix, args.public_key)
    stage_prefix = tempfile.mkdtemp(prefix=".cascade-plugin-stage-", dir=target_prefix)
    publish_operations = []
    transaction_committed = False
    try:
        stage = plugin_layout(stage_prefix)
        environment = dict(os.environ)
        environment.update({
            "CASCADE_PREFIX": os.path.realpath(_CLI_PREFIX),
            "CASCADE_PLUGIN_PREFIX": stage_prefix,
            "CASCADE_PLUGIN_PACKAGE": package,
            "CASCADE_PLUGIN_DIR": stage["cpp"],
            "CASCADE_PYPLUGIN_DIR": stage["python"],
            "CASCADE_PLUGIN_INCLUDE_DIR": stage["include"],
            "CASCADE_PLUGIN_TRUST_STORE": stage["trust_store"],
            "CASCADE_CORE_INCLUDE": os.path.join(os.path.realpath(_CLI_PREFIX), "include", "cascade"),
            "CASCADE_CORE_LIB": os.path.join(os.path.realpath(_CLI_PREFIX), "lib"),
        })
        environment.pop("CASCADE_PLUGIN_PRIVATE_KEY", None)
        environment.pop("CASCADE_PLUGIN_PUBLIC_KEY", None)
        if args.private_key:
            environment["CASCADE_PLUGIN_PRIVATE_KEY"] = os.path.realpath(os.path.expanduser(args.private_key))
        if args.public_key:
            environment["CASCADE_PLUGIN_PUBLIC_KEY"] = os.path.realpath(os.path.expanduser(args.public_key))

        command = [args.scons, "install", f"-j{args.jobs}"]
        if not args.json:
            _log("INFO", f"Building plugin {package} in {source}")
        completed = subprocess.run(
            command,
            cwd=source,
            env=environment,
            capture_output=args.json,
            text=args.json,
        )
        if completed.returncode != 0:
            if args.json and completed.stderr:
                _log("ERROR", completed.stderr.rstrip())
            raise RuntimeError(f"Plugin build failed with exit code {completed.returncode}")

        verification = _verify_staged_plugin(
            stage_prefix,
            package,
            bool(getattr(args, "require_signed", False)),
            trusted_key_snapshots,
            quiet=args.json,
        )
        operator_key = next(
            (entry for entry in trusted_key_snapshots if entry["operator_supplied"]),
            None,
        )
        publish_public_key = bool(
            operator_key
            and any(report.get("operator_signed") for report in verification.get("packages", []))
        )
        with _package_lock(target_prefix, package, exclusive=True):
            conflicts = _other_prefix_package_locations(package, target_prefix)
            if conflicts:
                raise RuntimeError(
                    "Plugin package is already installed under another active prefix: "
                    + ", ".join(conflicts)
                )
            _reject_prospective_duplicates(stage_prefix, target_prefix, package)
            publish_operations = _publish_staged_plugin(
                stage_prefix,
                target_prefix,
                package,
                operator_key["data"] if publish_public_key else None,
            )
            try:
                registered = add_plugin_prefix(target_prefix)
            except Exception as config_error:
                try:
                    _restore_publish_operations(publish_operations)
                except Exception as rollback_error:
                    raise RuntimeError(
                        f"Config update failed ({config_error}); rollback was incomplete: {rollback_error}; "
                        f"recovery data preserved at {os.path.join(stage_prefix, '.rollback')}"
                    ) from config_error
                raise
            transaction_committed = True
        published = [
            operation["destination"]
            for operation in publish_operations
            if operation["published"]
        ]
        target_layout = plugin_layout(target_prefix)
        verification["trust_store"] = target_layout["trust_store"]
        for report in verification.get("packages", []):
            staged_path = report.get("path", "")
            if staged_path.startswith(stage_prefix + os.sep):
                report["path"] = target_prefix + staged_path[len(stage_prefix):]
        _emit({
            "package": package,
            "source": source,
            "prefix": target_prefix,
            "registered_prefix": registered,
            "published": published,
            "verification": verification,
        }, args.json)
    finally:
        rollback_root = os.path.join(stage_prefix, ".rollback")
        recovery_data = (
            not transaction_committed
            and os.path.isdir(rollback_root)
            and bool(os.listdir(rollback_root))
        )
        if recovery_data:
            _log("ERROR", f"Plugin recovery data preserved at {rollback_root}")
        else:
            try:
                shutil.rmtree(stage_prefix)
            except FileNotFoundError:
                pass
            except OSError as cleanup_error:
                if sys.exc_info()[0] is None:
                    raise
                _log("WARN", f"Cannot remove plugin staging directory {stage_prefix}: {cleanup_error}")
