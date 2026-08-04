import os

from .common import _emit


def _cache_manager():
    from cascade import CacheManager
    return CacheManager


def _cache_directory(args) -> str:
    configured = getattr(args, "cache_directory", None)
    value = configured or _cache_manager().cache_dir()
    return os.path.abspath(os.path.expanduser(value))


def _snapshot_payload(snapshot):
    provenance = snapshot.provenance or None
    return {
        "module": snapshot.module,
        "hash": snapshot.hash,
        "provenance": provenance,
        "provenance_exists": os.path.isfile(provenance) if provenance else None,
        "cache_file": snapshot.cache_file,
    }


def cmd_cache_list(args) -> None:
    root = _cache_directory(args)
    snapshots = [
        _snapshot_payload(snapshot)
        for snapshot in _cache_manager().list_snapshots(root, getattr(args, "module", None) or "")
    ]
    payload = {"cache_directory": root, "snapshots": snapshots, "count": len(snapshots)}
    if args.json:
        _emit(payload, True)
        return
    if not snapshots:
        print(f"No cache snapshots found in {root}.")
        return
    for snapshot in snapshots:
        provenance = snapshot["provenance"] or "(not recorded)"
        if snapshot["provenance_exists"] is False:
            provenance += " [missing]"
        print(f"{snapshot['module']} {snapshot['hash']} -> {provenance}")


def cmd_cache_explain(args) -> None:
    root = _cache_directory(args)
    manager = _cache_manager()
    cached = manager.is_hash_cached(args.module, args.hash, root)
    provenance = manager.find_provenance(args.module, args.hash, root) if cached else ""
    payload = {
        "cache_directory": root,
        "module": args.module,
        "hash": args.hash,
        "cached": cached,
        "provenance": provenance or None,
        "provenance_exists": os.path.isfile(provenance) if provenance else None,
        "reason": "matching snapshot hash is cached" if cached else "snapshot hash is not present",
    }
    if args.json:
        _emit(payload, True)
        return
    print(f"{'HIT' if cached else 'MISS'}: {args.module} {args.hash}")
    print(payload["reason"])
    if provenance:
        suffix = "" if payload["provenance_exists"] else " (missing)"
        print(f"Provenance: {provenance}{suffix}")


def cmd_cache_prune(args) -> None:
    root = _cache_directory(args)
    removed = [
        _snapshot_payload(snapshot)
        for snapshot in _cache_manager().prune(
            root,
            getattr(args, "module", None) or "",
            bool(args.all),
            bool(args.dry_run),
        )
    ]
    changed_files = sorted({snapshot["cache_file"] for snapshot in removed})
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
