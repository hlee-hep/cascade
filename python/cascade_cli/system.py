import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
from typing import Any, Dict, List, Optional

from .common import _CLI_PREFIX, _emit, _log, _parse_kv
from .plugin import _default_plugin_dirs


def _framework_info() -> Dict[str, Any]:
    info: Dict[str, Any] = {
        "prefix": os.path.realpath(_CLI_PREFIX),
        "python": sys.version.split()[0],
    }
    try:
        import cascade

        version = getattr(cascade, "__version__", None)
        abi_version = getattr(cascade, "__abi_version__", None)
        abi_tag = getattr(cascade, "__abi_tag__", None)
        get_version = getattr(cascade, "get_version", lambda: "unavailable")
        get_abi_version = getattr(cascade, "get_abi_version", lambda: -1)
        get_abi_tag = getattr(cascade, "get_abi_tag", lambda: "unavailable")
        info.update(
            {
                "version": str(version if version is not None else get_version()),
                "abi_version": int(abi_version if abi_version is not None else get_abi_version()),
                "abi_tag": str(abi_tag if abi_tag is not None else get_abi_tag()),
                "module": os.path.realpath(cascade.__file__),
            }
        )
    except Exception as error:
        info["error"] = str(error)
    cpp_dir, py_dir = _default_plugin_dirs()
    info["cpp_plugin_dir"] = os.path.realpath(cpp_dir)
    info["python_plugin_dir"] = os.path.realpath(py_dir)
    info["trust_store"] = os.path.realpath(
        os.environ.get(
            "CASCADE_PLUGIN_TRUST_STORE",
            os.path.join(_CLI_PREFIX, "share", "cascade", "trusted_keys"),
        )
    )
    return info


def cmd_info(args) -> None:
    info = _framework_info()
    _emit(info, args.json)
    if "error" in info:
        raise SystemExit(1)


def cmd_doctor_env(args) -> None:
    checks = []

    def check(name: str, status: str, detail: str) -> None:
        checks.append({"name": name, "status": status, "detail": detail})

    info = _framework_info()
    if "error" in info:
        check("cascade", "ERROR", info["error"])
    else:
        check("cascade", "OK", f"version {info['version']}, ABI {info['abi_version']}")
        module_path = info["module"]
        prefix_path = info["prefix"]
        if os.path.commonpath([module_path, prefix_path]) != prefix_path:
            check("installation coherence", "WARN", f"CLI prefix {prefix_path} imports {module_path}")

    root = shutil.which(args.root_exe)
    check("ROOT", "OK" if root else "WARN", root or f"{args.root_exe!r} was not found on PATH")
    openssl = shutil.which("openssl")
    check("OpenSSL", "OK" if openssl else "ERROR", openssl or "openssl was not found on PATH")

    for name, path in (
        ("C++ plugin root", info["cpp_plugin_dir"]),
        ("Python plugin root", info["python_plugin_dir"]),
        ("Trust store", info["trust_store"]),
    ):
        check(name, "OK" if os.path.isdir(path) else "WARN", path)

    if args.json:
        _emit({"checks": checks}, True)
    else:
        for item in checks:
            _log(item["status"], f"{item['name']}: {item['detail']}")
    if any(item["status"] == "ERROR" for item in checks):
        raise SystemExit(1)


def _root_invoke(macro: str,
                 json_params_path: Optional[str],
                 extra_args: List[str],
                 root_exe: str = "root",
                 use_plus: bool = False) -> None:
    if not os.path.exists(macro):
        raise FileNotFoundError(f"macro not found: {macro}")
    macro_spec = f'{macro}{"+" if use_plus else ""}'
    arglist: List[str] = []
    if json_params_path is not None:
        arglist.append(json_params_path)
    arglist.extend(extra_args or [])
    quoted_with_dq = ",".join(json.dumps(str(a), ensure_ascii=False) for a in arglist)
    cmd = [root_exe, "-l", "-q", f'{macro_spec}({quoted_with_dq})']
    _log("INFO", "[ROOT] " + " ".join(shlex.quote(c) for c in cmd))
    completed = subprocess.run(cmd)
    if completed.returncode != 0:
        _log("ERROR", f"ROOT macro exited with code {completed.returncode}")
        raise SystemExit(completed.returncode)


def cmd_macro_run(args) -> None:
    params: Dict[str, Any] = {}
    if args.yaml:
        if not os.path.isfile(args.yaml):
            raise FileNotFoundError(f"YAML configuration not found: {args.yaml}")
        params["_yaml_path"] = os.path.abspath(args.yaml)
    if args.set:
        params.update(dict(_parse_kv(s) for s in args.set))
    json_path: Optional[str] = None
    if params:
        fd, json_path = tempfile.mkstemp(prefix="cascade_params_", suffix=".json")
        os.close(fd)
        with open(json_path, "w", encoding="utf-8") as jf:
            json.dump(params, jf, ensure_ascii=False, indent=2)
        _log("DEBUG", f"Wrote temporary params JSON: {json_path}")
    try:
        _root_invoke(
            macro=args.macro,
            json_params_path=json_path,
            extra_args=args.extra or [],
            root_exe=args.root_exe,
            use_plus=not args.no_plus
        )
    finally:
        if json_path and os.path.exists(json_path):
            os.remove(json_path)
            _log("DEBUG", f"Removed temporary file: {json_path}")
