import contextlib
import os
import re
import sys
import tempfile
from typing import Any, Dict, Iterable, List

from .common import _emit


def _default_cache_directory() -> str:
    configured = os.environ.get("CASCADE_CACHE_DIR")
    if configured:
        return os.path.abspath(os.path.expanduser(configured))
    return os.path.join(os.path.expanduser("~"), ".cache", "cascade", "snapshot_cache")


def _cache_directory(args) -> str:
    value = getattr(args, "cache_directory", None) or _default_cache_directory()
    return os.path.abspath(os.path.expanduser(value))


def _safe_module_name(value: str) -> str:
    safe = re.sub(r"[^A-Za-z0-9_-]", "_", value)
    return safe or "unnamed"


def _cache_files(root: str, module: str = None) -> Iterable[str]:
    if module:
        path = os.path.join(root, _safe_module_name(module) + ".yaml")
        if os.path.isfile(path) and not os.path.islink(path):
            yield path
        python_path = os.path.join(root, "python_modules.json")
        if os.path.isfile(python_path) and not os.path.islink(python_path):
            yield python_path
        return
    if not os.path.isdir(root):
        return
    for name in sorted(os.listdir(root)):
        path = os.path.join(root, name)
        if (name.endswith(".yaml") or name == "python_modules.json") and os.path.isfile(path) and not os.path.islink(path):
            yield path


@contextlib.contextmanager
def _cache_lock(path: str, exclusive: bool):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    descriptor = os.open(path + ".lock", os.O_CREAT | os.O_RDWR, 0o600)
    try:
        if os.name != "nt":
            import fcntl
            fcntl.flock(descriptor, fcntl.LOCK_EX if exclusive else fcntl.LOCK_SH)
        yield
    finally:
        if os.name != "nt":
            import fcntl
            fcntl.flock(descriptor, fcntl.LOCK_UN)
        os.close(descriptor)


def _load_document(path: str) -> Dict[str, Any]:
    try:
        import yaml
    except ImportError as error:
        raise RuntimeError("PyYAML is required to manage Cascade caches") from error
    with open(path, "r", encoding="utf-8") as source:
        value = yaml.safe_load(source)
    if value is None:
        return {"schema_version": 1, "snapshots": []}
    if isinstance(value, list):
        if not all(isinstance(item, str) for item in value):
            raise ValueError(f"legacy cache entries must be hashes: {path}")
        return {
            "schema_version": 1,
            "snapshots": [{"hash": item, "provenance": ""} for item in value],
        }
    if not isinstance(value, dict) or value.get("schema_version") != 1:
        raise ValueError(f"unsupported Cascade cache schema: {path}")
    snapshots = value.get("snapshots")
    if not isinstance(snapshots, list):
        raise ValueError(f"Cascade cache snapshots must be a list: {path}")
    normalized = []
    for index, entry in enumerate(snapshots):
        if not isinstance(entry, dict) or not isinstance(entry.get("hash"), str):
            raise ValueError(f"invalid Cascade cache snapshot {index}: {path}")
        provenance = entry.get("provenance", "")
        if provenance is None:
            provenance = ""
        if not isinstance(provenance, str):
            raise ValueError(f"invalid cache provenance path at snapshot {index}: {path}")
        module = entry.get("module", "")
        if module is None:
            module = ""
        if not isinstance(module, str):
            raise ValueError(f"invalid cache module name at snapshot {index}: {path}")
        normalized.append({"hash": entry["hash"], "provenance": provenance, "module": module})
    return {"schema_version": 1, "snapshots": normalized}


def _write_document(path: str, document: Dict[str, Any]) -> None:
    try:
        import yaml
    except ImportError as error:
        raise RuntimeError("PyYAML is required to manage Cascade caches") from error
    descriptor, temporary = tempfile.mkstemp(
        prefix=os.path.basename(path) + ".tmp.", dir=os.path.dirname(path), text=True
    )
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as output:
            if path.endswith(".json"):
                import json
                json.dump(document, output, indent=2)
                output.write("\n")
            else:
                yaml.safe_dump(document, output, sort_keys=False)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.remove(temporary)


def _snapshot_module(snapshot: Dict[str, Any], path: str) -> str:
    if snapshot.get("module"):
        return snapshot["module"]
    if not path.endswith(".json"):
        return os.path.basename(path)[:-5]
    provenance = snapshot.get("provenance")
    if provenance and os.path.isfile(provenance):
        try:
            import json
            with open(provenance, "r", encoding="utf-8") as source:
                manifest = json.load(source)
            instance = manifest.get("module", {}).get("instance")
            if isinstance(instance, str) and instance:
                return instance
        except (OSError, ValueError, TypeError):
            pass
    return "python_modules"


def _scan_entries(root: str, module: str = None):
    entries = []
    errors = []
    for path in _cache_files(root, module):
        try:
            with _cache_lock(path, exclusive=False):
                document = _load_document(path)
        except Exception as error:
            errors.append({"cache_file": path, "error": str(error)})
            continue
        modified_at = os.path.getmtime(path)
        for snapshot in document["snapshots"]:
            provenance = snapshot["provenance"]
            module_name = _snapshot_module(snapshot, path)
            if module and module_name not in (module, _safe_module_name(module)):
                continue
            entries.append({
                "module": module_name,
                "hash": snapshot["hash"],
                "provenance": provenance or None,
                "provenance_exists": os.path.isfile(provenance) if provenance else None,
                "cache_file": path,
                "modified_at": modified_at,
            })
    return entries, errors


def _entries(root: str, module: str = None) -> List[Dict[str, Any]]:
    entries, errors = _scan_entries(root, module)
    if errors:
        raise ValueError(errors[0]["error"])
    return entries


def cmd_cache_list(args) -> None:
    root = _cache_directory(args)
    entries, errors = _scan_entries(root, getattr(args, "module", None))
    payload = {
        "cache_directory": root,
        "snapshots": entries,
        "count": len(entries),
        "errors": errors,
    }
    if args.json:
        _emit(payload, True)
    else:
        if not entries:
            print(f"No cache snapshots found in {root}.")
        for entry in entries:
            provenance = entry["provenance"] or "(not recorded)"
            if entry["provenance_exists"] is False:
                provenance += " [missing]"
            print(f"{entry['module']} {entry['hash']} -> {provenance}")
        for error in errors:
            print(f"Invalid cache file {error['cache_file']}: {error['error']}", file=sys.stderr)
    if errors:
        raise SystemExit(1)


def cmd_cache_explain(args) -> None:
    root = _cache_directory(args)
    entries = [entry for entry in _entries(root, args.module) if entry["hash"] == args.hash]
    entry = entries[0] if entries else None
    payload = {
        "cache_directory": root,
        "module": args.module,
        "hash": args.hash,
        "cached": entry is not None,
        "provenance": entry["provenance"] if entry else None,
        "provenance_exists": entry["provenance_exists"] if entry else None,
        "reason": "matching snapshot hash is cached" if entry else "snapshot hash is not present",
    }
    if args.json:
        _emit(payload, True)
        return
    verdict = "HIT" if entry else "MISS"
    print(f"{verdict}: {args.module} {args.hash}")
    print(payload["reason"])
    if entry and entry["provenance"]:
        suffix = "" if entry["provenance_exists"] else " (missing)"
        print(f"Provenance: {entry['provenance']}{suffix}")


def cmd_cache_prune(args) -> None:
    root = _cache_directory(args)
    removed = []
    changed_files = []
    for path in list(_cache_files(root, getattr(args, "module", None))):
        with _cache_lock(path, exclusive=True):
            document = _load_document(path)
            kept = []
            for snapshot in document["snapshots"]:
                provenance = snapshot["provenance"]
                module_name = _snapshot_module(snapshot, path)
                requested_module = getattr(args, "module", None)
                if requested_module and module_name not in (
                    requested_module, _safe_module_name(requested_module)
                ):
                    kept.append(snapshot)
                    continue
                should_remove = bool(args.all) or bool(provenance and not os.path.isfile(provenance))
                if should_remove:
                    removed.append({
                        "module": module_name,
                        "hash": snapshot["hash"],
                        "provenance": provenance or None,
                    })
                else:
                    kept.append(snapshot)
            if len(kept) != len(document["snapshots"]):
                changed_files.append(path)
                if not args.dry_run:
                    _write_document(path, {"schema_version": 1, "snapshots": kept})
    payload = {
        "cache_directory": root,
        "dry_run": bool(args.dry_run),
        "mode": "all" if args.all else "missing-provenance",
        "removed": removed,
        "removed_count": len(removed),
        "changed_files": changed_files,
    }
    if args.json:
        _emit(payload, True)
        return
    action = "Would remove" if args.dry_run else "Removed"
    print(f"{action} {len(removed)} snapshot(s) from {len(changed_files)} cache file(s).")
