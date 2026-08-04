import json
import os
from typing import Any, Dict, List, Optional, Tuple

from .common import (
    _apply_parameters,
    _emit,
    _load_controller,
    _parse_kv,
    _redirect_stdout_to_stderr,
    _result_payload,
)


_PROVENANCE_SCHEMAS = {
    "cascade.module-run": "module",
    "cascade.workflow-run": "workflow",
}
_VOLATILE_PROVENANCE_KEYS = {
    "run_id",
    "timing",
    "manifest_path",
    "module_run_id",
    "module_manifest",
    "module_manifests",
    "cache_source_manifest",
}


def _default_provenance_roots() -> List[str]:
    cache_root = os.environ.get(
        "CASCADE_CACHE_DIR",
        os.path.join(os.path.expanduser("~"), ".cache", "cascade"),
    )
    return [os.getcwd(), cache_root]


def _load_provenance_manifest(path: str) -> Dict[str, Any]:
    resolved = os.path.realpath(os.path.expanduser(path))
    with open(resolved, "r", encoding="utf-8") as source:
        manifest = json.load(source)
    if not isinstance(manifest, dict):
        raise TypeError(f"provenance manifest root must be an object: {resolved}")
    schema = manifest.get("schema")
    if schema not in _PROVENANCE_SCHEMAS or manifest.get("schema_version") != 1:
        raise ValueError(f"unsupported Cascade provenance manifest: {resolved}")
    return manifest


def _find_provenance_manifests(roots: Optional[List[str]] = None) -> List[Tuple[str, Dict[str, Any]]]:
    manifests = []
    visited = set()
    for root in roots or _default_provenance_roots():
        resolved_root = os.path.realpath(os.path.expanduser(root))
        candidates = []
        if os.path.isfile(resolved_root):
            candidates.append(resolved_root)
        elif os.path.isdir(resolved_root):
            for directory, subdirectories, filenames in os.walk(resolved_root):
                subdirectories[:] = [
                    name for name in subdirectories
                    if name not in {".git", ".venv", "__pycache__", "node_modules"}
                ]
                candidates.extend(
                    os.path.join(directory, name)
                    for name in filenames
                    if name.endswith(".json")
                )
        for candidate in candidates:
            resolved = os.path.realpath(candidate)
            if resolved in visited:
                continue
            visited.add(resolved)
            try:
                manifests.append((resolved, _load_provenance_manifest(resolved)))
            except (OSError, ValueError, TypeError, json.JSONDecodeError):
                continue
    return manifests


def _resolve_provenance_manifest(reference: str, roots: Optional[List[str]] = None) -> Tuple[str, Dict[str, Any]]:
    expanded = os.path.realpath(os.path.expanduser(reference))
    if os.path.isfile(expanded):
        return expanded, _load_provenance_manifest(expanded)
    matches = [
        (path, manifest)
        for path, manifest in _find_provenance_manifests(roots)
        if manifest.get("run_id") == reference
    ]
    if not matches:
        raise FileNotFoundError(f"Cascade run not found: {reference}")
    if len(matches) > 1:
        paths = ", ".join(path for path, _ in matches)
        raise RuntimeError(f"Cascade run ID is ambiguous: {reference} ({paths})")
    return matches[0]


def _manifest_summary(path: str, manifest: Dict[str, Any]) -> Dict[str, Any]:
    schema = manifest["schema"]
    if schema == "cascade.module-run":
        module = manifest.get("module", {})
        subject = module.get("instance") or module.get("name") or "unknown"
        status = manifest.get("result", {}).get("status", "Unknown")
    else:
        nodes = manifest.get("dag", {}).get("nodes", [])
        subject = f"{len(nodes)} nodes"
        status = "Succeeded" if manifest.get("execution", {}).get("succeeded") else "Failed"
    timing = manifest.get("timing", {})
    return {
        "run_id": manifest.get("run_id", ""),
        "kind": _PROVENANCE_SCHEMAS[schema],
        "subject": subject,
        "status": status,
        "started_at": timing.get("started_at", ""),
        "finished_at": timing.get("finished_at", ""),
        "manifest": path,
    }


def cmd_history(args) -> None:
    entries = [
        _manifest_summary(path, manifest)
        for path, manifest in _find_provenance_manifests(args.root)
        if args.kind == "all" or _PROVENANCE_SCHEMAS[manifest["schema"]] == args.kind
    ]
    entries.sort(key=lambda entry: (entry["started_at"], entry["run_id"]), reverse=True)
    entries = entries[:args.limit]
    if args.json:
        _emit({"runs": entries}, True)
        return
    if not entries:
        print("No Cascade runs found.")
        return
    for entry in entries:
        print(
            f"{entry['started_at'] or '-'}  {entry['status']:<11} "
            f"{entry['kind']:<8} {entry['subject']}  {entry['run_id']}"
        )
        print(f"  {entry['manifest']}")


def _print_manifest(manifest: Dict[str, Any]) -> None:
    schema = manifest["schema"]
    print(f"Run: {manifest.get('run_id', '')}")
    print(f"Type: {_PROVENANCE_SCHEMAS[schema]}")
    timing = manifest.get("timing", {})
    print(f"Started: {timing.get('started_at', '')}")
    print(f"Finished: {timing.get('finished_at', '')}")
    if schema == "cascade.module-run":
        module = manifest.get("module", {})
        result = manifest.get("result", {})
        execution = manifest.get("execution", {})
        print(f"Module: {module.get('name', '')} ({module.get('instance', '')})")
        plugin = manifest.get("plugin")
        if isinstance(plugin, dict):
            signer = plugin.get("signer_fingerprint") or "-"
            print(f"Plugin: {plugin.get('package', '')} ({plugin.get('trust', '')}, signer={signer})")
        print(f"Result: {result.get('status', '')} ({result.get('phase', '')})")
        if result.get("message"):
            print(f"Message: {result['message']}")
        print(f"Cache hit: {bool(execution.get('cache_hit'))}")
        print("Parameters:")
        print(json.dumps(manifest.get("parameters", {}), ensure_ascii=False, indent=2, sort_keys=True))
        artifacts = manifest.get("artifacts", {})
        for kind in ("inputs", "outputs"):
            print(f"{kind.title()}:")
            for artifact in artifacts.get(kind, []):
                digest = artifact.get("sha256") or "-"
                print(f"  {artifact.get('path', '')}  {artifact.get('kind', '')}  {digest}")
    else:
        execution = manifest.get("execution", {})
        print(f"Result: {'Succeeded' if execution.get('succeeded') else 'Failed'}")
        print(f"Fail fast: {bool(execution.get('fail_fast'))}")
        print("Nodes:")
        for node in manifest.get("dag", {}).get("nodes", []):
            dependencies = ", ".join(node.get("dependencies", [])) or "-"
            detail = f": {node.get('message')}" if node.get("message") else ""
            print(f"  {node.get('name', '')}: {node.get('status', '')} [{dependencies}]{detail}")
    print(f"Manifest: {manifest.get('manifest_path', '')}")


def cmd_inspect(args) -> None:
    _, manifest = _resolve_provenance_manifest(args.run, args.root)
    if args.json:
        _emit(manifest, True)
    else:
        _print_manifest(manifest)


def _stable_provenance(value: Any) -> Any:
    if isinstance(value, dict):
        return {
            key: _stable_provenance(item)
            for key, item in value.items()
            if key not in _VOLATILE_PROVENANCE_KEYS
        }
    if isinstance(value, list):
        return [_stable_provenance(item) for item in value]
    return value


def _provenance_changes(before: Any, after: Any, path: str = "") -> List[Dict[str, Any]]:
    if isinstance(before, dict) and isinstance(after, dict):
        changes = []
        for key in sorted(set(before) | set(after)):
            child_path = f"{path}.{key}" if path else key
            if key not in before:
                changes.append({"path": child_path, "before": "<missing>", "after": after[key]})
            elif key not in after:
                changes.append({"path": child_path, "before": before[key], "after": "<missing>"})
            else:
                changes.extend(_provenance_changes(before[key], after[key], child_path))
        return changes
    if before != after:
        return [{"path": path, "before": before, "after": after}]
    return []


def cmd_diff(args) -> None:
    before_path, before = _resolve_provenance_manifest(args.before, args.root)
    after_path, after = _resolve_provenance_manifest(args.after, args.root)
    changes = _provenance_changes(_stable_provenance(before), _stable_provenance(after))
    payload = {
        "before": {"run_id": before.get("run_id", ""), "manifest": before_path},
        "after": {"run_id": after.get("run_id", ""), "manifest": after_path},
        "changes": changes,
    }
    if args.json:
        _emit(payload, True)
        return
    print(f"Before: {payload['before']['run_id']} ({before_path})")
    print(f"After:  {payload['after']['run_id']} ({after_path})")
    if not changes:
        print("No reproducibility-relevant differences.")
        return
    for change in changes:
        before_text = json.dumps(change["before"], ensure_ascii=False, sort_keys=True)
        after_text = json.dumps(change["after"], ensure_ascii=False, sort_keys=True)
        print(f"{change['path']}: {before_text} -> {after_text}")


def _sensitive_parameter(key: str) -> bool:
    lowered = key.lower()
    return any(
        pattern in lowered
        for pattern in (
            "password", "passwd", "secret", "token", "credential", "private_key", "api_key",
        )
    )


def cmd_replay(args) -> None:
    _, manifest = _resolve_provenance_manifest(args.run, args.root)
    if manifest["schema"] != "cascade.module-run":
        raise ValueError("replay currently supports module runs; replay DAGs from their workflow file")
    parameters = manifest.get("parameters", {})
    if not isinstance(parameters, dict):
        raise TypeError("module provenance parameters must be an object")
    overrides = dict(_parse_kv(value) for value in args.set)
    redacted = sorted(
        key for key, value in parameters.items()
        if _sensitive_parameter(key) and value == "***" and key not in overrides
    )
    if redacted:
        options = " ".join(f"--set {key}=VALUE" for key in redacted)
        raise ValueError(f"replay requires redacted parameters to be supplied again: {options}")

    module = manifest.get("module", {})
    module_name = module.get("name")
    instance_name = args.name or module.get("instance") or None
    if not module_name:
        raise ValueError("module provenance does not contain a module name")
    directories = manifest.get("directories", {})
    output_directory = args.output_directory or directories.get("output")
    cache_directory = args.cache_directory or directories.get("cache")
    isolated = args.isolated
    if isolated is None:
        isolated = bool(manifest.get("execution", {}).get("isolated"))

    with _redirect_stdout_to_stderr(args.json):
        controller = _load_controller(args.json, getattr(args, "require_signed", False))
        handle = controller.register_module(module_name, instance_name)
        if output_directory:
            handle.set_output_directory(os.path.abspath(output_directory))
        if cache_directory:
            handle.set_cache_directory(os.path.abspath(cache_directory))
        replay_parameters = {
            key: value
            for key, value in parameters.items()
            if not (_sensitive_parameter(key) and value == "***")
        }
        replay_parameters.update(overrides)
        _apply_parameters(handle, replay_parameters)
        result = controller.run_module(handle.name(), isolated=isolated)
    payload = _result_payload(result, handle.name())
    payload["run_id"] = handle.get_run_id()
    payload["provenance"] = handle.get_last_provenance_path()
    payload["replayed_from"] = manifest.get("run_id", "")
    if args.json:
        _emit(payload, True)
    else:
        detail = f": {payload['message']}" if payload["message"] else ""
        print(f"{payload['name']}: {payload['status']} ({payload['phase']}){detail}")
        print(f"Replayed from: {payload['replayed_from']}")
        print(f"Provenance: {payload['provenance']}")
    if not payload["allows_dependents"]:
        raise SystemExit(1)
