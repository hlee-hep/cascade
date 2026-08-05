import contextlib
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

from .common import _CLI_PREFIX, _emit, _load_mapping, _log, _validate_keys

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


def _status_text(value: Any) -> str:
    return getattr(value, "name", str(value).rsplit(".", 1)[-1])


def _sha256_file(path: str) -> str:
    from cascade._cascade import PluginVerifier
    return PluginVerifier.hash_file(path)


def _sha256_bytes(data: bytes) -> str:
    from cascade._cascade import PluginVerifier
    return PluginVerifier.hash_bytes(data)


def _read_regular_file(path: str) -> bytes:
    from cascade._cascade import PluginVerifier
    return PluginVerifier.read_file(path)


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
                "fingerprint": _sha256_bytes(data),
                "operator_supplied": False,
            })
    if public_key:
        path = os.path.realpath(os.path.abspath(os.path.expanduser(public_key)))
        data = _read_regular_file(path)
        fingerprint = _sha256_bytes(data)
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
            safe_name = re.sub(r"[^A-Za-z0-9_.-]", "_", snapshot.get("name", "key.pem"))
            marker = "operator-" if snapshot.get("operator_supplied") else ""
            path = os.path.join(directory, f"{marker}{index:04d}-{safe_name}")
            descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(descriptor, "wb") as output:
                output.write(snapshot["data"])
            paths.append(path)
            snapshot_by_path[path] = snapshot
        yield paths, snapshot_by_path


def _core_verify_package(
    path: str,
    language: str,
    trusted_keys: List[str],
    require_signed: bool,
):
    from cascade._cascade import PluginTrustPolicy, PluginVerifier

    package = os.path.basename(os.path.abspath(path))
    package_key = next(
        (
            key
            for key in trusted_keys
            if re.sub(r"^\d{4}-", "", os.path.basename(key)) == package + ".pem"
        ),
        "",
    )
    operator_key = next(
        (
            key
            for key in trusted_keys
            if package_key == "" and os.path.basename(key).startswith("operator-")
        ),
        "",
    )
    policy = PluginTrustPolicy.RequireSigned if require_signed else PluginTrustPolicy.Verified
    trust_store = os.path.dirname(package_key or operator_key) if (package_key or operator_key) else ""
    return PluginVerifier.verify_package(
        path,
        trust_store,
        policy,
        language,
        package_key or operator_key,
    )


def _ensure_real_directory_tree(path: str, allow_missing_leaf: bool = False) -> None:
    from cascade._cascade import PluginVerifier
    PluginVerifier.validate_directory_tree(path, allow_missing_leaf)


def _validate_staged_tree(package_dir: str) -> None:
    from cascade._cascade import PluginVerifier
    PluginVerifier.validate_staged_tree(package_dir)


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
    from cascade._cascade import PluginPaths
    return [
        {
            "prefix": layout.prefix,
            "cpp": layout.cpp,
            "python": layout.python,
            "include": layout.include,
            "trust_store": layout.trust_store,
            "source": layout.source,
        }
        for layout in PluginPaths.runtime_layouts()
    ]


def _doctor_plugin_package(
    path: str,
    language: str,
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
            _log(level, message, "PLUGIN")

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

    try:
        package = _core_verify_package(path, language, trusted_keys, require_signed)
    except Exception as error:
        emit("ERROR", f"[{language}] {error}")
        errors += 1
        return finish()

    trust = _status_text(package.trust).upper()
    verified_key = package.trusted_key_path or None
    for artifact in package.artifacts:
        identities = list(artifact.classes) if language == "python" else [artifact.name]
        names.extend(identities)
        emit("INFO", f"[{language}] {artifact.name}: hash OK")
    emit(trust, f"[{language}] {package.package} modules={len(package.artifacts)} errors=0 warnings=0")
    return finish()

def _doctor_plugin_dir(
    path: str,
    language: str,
    trusted_keys: List[str],
    require_signed: bool = False,
    reports: Optional[List[Dict[str, Any]]] = None,
    quiet: bool = False,
) -> Tuple[int, List[str]]:
    if not quiet:
        _log("INFO", f"[{language}] package root: {path}", "PLUGIN")
    if not os.path.isdir(path):
        if not quiet:
            _log("WARNING", f"[{language}] package root not found", "PLUGIN")
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
            _log("WARNING", f"no trusted plugin keys found in {trust_store}", "PLUGIN")

        report_start = len(reports)
        cpp_errors, layout_cpp_names = (0, [])
        py_errors, layout_py_names = (0, [])
        if layout["cpp"]:
            cpp_errors, layout_cpp_names = _doctor_plugin_dir(
                layout["cpp"], "cpp", trusted_keys, require_signed, reports, quiet
            )
        if layout["python"]:
            py_errors, layout_py_names = _doctor_plugin_dir(
                layout["python"], "python", trusted_keys, require_signed, reports, quiet
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
            _log("ERROR", f"duplicate plugin module names: {duplicates}", "PLUGIN")
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
        for language, root in (
            ("cpp", layout["cpp"]),
            ("python", layout["python"]),
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


def _sign_staged_manifests(stage_prefix: str, package: str, private_key: str) -> None:
    layout = plugin_layout(stage_prefix)
    key = os.path.realpath(os.path.abspath(os.path.expanduser(private_key)))
    _read_regular_file(key)
    manifests = [
        os.path.join(layout[language], package, "plugin_manifest.json")
        for language in ("cpp", "python")
        if os.path.isfile(os.path.join(layout[language], package, "plugin_manifest.json"))
    ]
    for manifest in manifests:
        subprocess.run(
            [
                "openssl",
                "pkeyutl",
                "-sign",
                "-inkey",
                key,
                "-rawin",
                "-in",
                manifest,
                "-out",
                manifest + ".sig",
            ],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )


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


_PLUGIN_CONFIG_NAMES = ("cascade-plugin.yaml", "cascade-plugin.yml", "cascade-plugin.json")


def _plugin_build_template() -> str:
    candidates = (
        os.path.join(_CLI_PREFIX, "share", "cascade", "scripts", "plugin_sconstruct"),
        os.path.join(_CLI_PREFIX, "scripts", "plugin_sconstruct"),
    )
    for candidate in candidates:
        if os.path.isfile(candidate):
            return os.path.realpath(candidate)
    raise FileNotFoundError(
        "Cascade plugin build template is missing; reinstall Cascade before building convention plugins"
    )


def _source_file_stems(directory: str, suffix: str) -> List[str]:
    if not os.path.isdir(directory):
        return []
    return sorted(
        os.path.splitext(name)[0]
        for name in os.listdir(directory)
        if name.endswith(suffix) and os.path.isfile(os.path.join(directory, name))
    )


def _reject_removed_placeholders(source: str) -> None:
    removed = ("@BASENAME@", "@VERSION_HASH@")
    for directory, suffix in (("include", ".hh"), ("src", ".cc"), ("python", ".py")):
        root = os.path.join(source, directory)
        if not os.path.isdir(root):
            continue
        for name in sorted(os.listdir(root)):
            path = os.path.join(root, name)
            if not name.endswith(suffix) or not os.path.isfile(path):
                continue
            with open(path, "r", encoding="utf-8") as plugin_source:
                content = plugin_source.read()
            tokens = [token for token in removed if token in content]
            if tokens:
                raise ValueError(
                    f"Plugin source uses removed build placeholders in {path}: {', '.join(tokens)}"
                )


def _load_convention_build(source: str) -> Dict[str, Any]:
    config_paths = [
        os.path.join(source, name)
        for name in _PLUGIN_CONFIG_NAMES
        if os.path.isfile(os.path.join(source, name))
    ]
    if len(config_paths) > 1:
        raise ValueError("Plugin source contains multiple cascade-plugin configuration files")
    config = _load_mapping(config_paths[0]) if config_paths else {}
    _validate_keys(config, {"schema_version", "root_modules", "class_map"}, "plugin configuration")
    if config_paths and config.get("schema_version") != 1:
        raise ValueError("Plugin configuration requires schema_version: 1")
    _reject_removed_placeholders(source)

    headers = _source_file_stems(os.path.join(source, "include"), ".hh")
    sources = _source_file_stems(os.path.join(source, "src"), ".cc")
    python_sources = [
        stem
        for stem in _source_file_stems(os.path.join(source, "python"), ".py")
        if stem != "__init__"
    ]
    if headers != sources:
        missing_sources = sorted(set(headers) - set(sources))
        missing_headers = sorted(set(sources) - set(headers))
        details = []
        if missing_sources:
            details.append("missing src/*.cc for " + ", ".join(missing_sources))
        if missing_headers:
            details.append("missing include/*.hh for " + ", ".join(missing_headers))
        raise ValueError("Invalid convention plugin layout: " + "; ".join(details))
    if not headers and not python_sources:
        raise ValueError(
            "Convention plugin source must contain matching include/*.hh and src/*.cc files or python/*.py files"
        )
    invalid_cpp_names = [name for name in headers if not name.endswith("Module")]
    if invalid_cpp_names:
        raise ValueError("C++ plugin source stems must end in Module: " + ", ".join(invalid_cpp_names))

    root_modules = config.get("root_modules", [])
    if not isinstance(root_modules, list) or any(
        not isinstance(name, str) or not name for name in root_modules
    ):
        raise TypeError("plugin configuration root_modules must be a list of non-empty strings")
    if len(root_modules) != len(set(root_modules)):
        raise ValueError("plugin configuration root_modules contains duplicates")
    if "*" in root_modules and len(root_modules) != 1:
        raise ValueError("plugin configuration root_modules '*' must be the only entry")
    unknown_root_modules = sorted(set(root_modules) - set(headers) - {"*"})
    if unknown_root_modules:
        raise ValueError(
            "plugin configuration names unknown ROOT modules: " + ", ".join(unknown_root_modules)
        )

    class_map = config.get("class_map", {})
    if not isinstance(class_map, dict) or any(
        not isinstance(stem, str) or not isinstance(class_name, str) or not class_name
        for stem, class_name in class_map.items()
    ):
        raise TypeError(
            "plugin configuration class_map must map source stems to non-empty class names"
        )
    unknown_class_map = sorted(set(class_map) - set(headers))
    if unknown_class_map:
        raise ValueError(
            "plugin configuration class_map names unknown modules: " + ", ".join(unknown_class_map)
        )
    class_pattern = re.compile(r"[A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z_][A-Za-z0-9_]*)*")
    invalid_classes = sorted(name for name in class_map.values() if not class_pattern.fullmatch(name))
    if invalid_classes:
        raise ValueError(
            "plugin configuration class_map contains invalid C++ class names: " + ", ".join(invalid_classes)
        )

    return {
        "template": _plugin_build_template(),
        "root_modules": root_modules,
        "class_map": class_map,
    }


def cmd_plugin_install(args) -> None:
    source = os.path.realpath(os.path.abspath(os.path.expanduser(args.source)))
    if not os.path.isdir(source):
        raise FileNotFoundError(f"Plugin source directory not found: {source}")
    if os.path.lexists(os.path.join(source, "SConstruct")):
        raise ValueError(
            "Package-owned SConstruct files are no longer supported; remove it and use cascade-plugin.yaml"
        )
    convention_build = _load_convention_build(source)
    package = args.package or os.path.basename(source)
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", package):
        raise ValueError("Plugin package must contain only letters, digits, '.', '_', or '-'")
    if bool(args.private_key) != bool(args.public_key):
        raise ValueError("--private-key and --public-key must be provided together")
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

        environment["CASCADE_PLUGIN_ROOT_MODULES"] = ",".join(convention_build["root_modules"])
        environment["CASCADE_PLUGIN_CLASS_MAP"] = json.dumps(
            convention_build["class_map"], sort_keys=True
        )
        command = [args.scons, "-f", convention_build["template"], "install", f"-j{args.jobs}"]
        if not args.json:
            _log("INFO", f"Building plugin {package} in {source}", "PLUGIN")
        completed = subprocess.run(
            command,
            cwd=source,
            env=environment,
            capture_output=args.json,
            text=args.json,
        )
        if completed.returncode != 0:
            if args.json and completed.stderr:
                _log("ERROR", completed.stderr.rstrip(), "PLUGIN")
            raise RuntimeError(f"Plugin build failed with exit code {completed.returncode}")

        if args.private_key:
            _sign_staged_manifests(stage_prefix, package, args.private_key)

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
            _log("ERROR", f"Plugin recovery data preserved at {rollback_root}", "PLUGIN")
        else:
            try:
                shutil.rmtree(stage_prefix)
            except FileNotFoundError:
                pass
            except OSError as cleanup_error:
                if sys.exc_info()[0] is None:
                    raise
                _log("WARNING", f"Cannot remove plugin staging directory {stage_prefix}: {cleanup_error}", "PLUGIN")
