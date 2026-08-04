import argparse
import os

from .common import _parse_kv, _positive_int
from .cache import cmd_cache_explain, cmd_cache_list, cmd_cache_prune
from .execution import cmd_dag_run, cmd_dag_validate, cmd_module_list, cmd_module_run
from .plugin import (
    cmd_doctor_plugins,
    cmd_plugin_install,
    cmd_plugin_path_add,
    cmd_plugin_path_list,
    cmd_plugin_path_remove,
)
from .provenance import cmd_diff, cmd_history, cmd_inspect, cmd_replay
from .system import _framework_info, cmd_doctor_env, cmd_info, cmd_macro_run


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="cascade", description="Cascade command line tools")
    version_info = _framework_info()
    version = version_info.get("version", "unavailable")
    abi = version_info.get("abi_version", "unavailable")
    p.add_argument("--version", action="version", version=f"Cascade {version} (plugin ABI {abi})")
    p.add_argument(
        "--require-signed",
        action="store_true",
        help="Require every discovered plugin package to have a trusted signature",
    )
    sub = p.add_subparsers(dest="command")

    info = sub.add_parser("info", help="Show framework version, ABI, and installation paths")
    info.add_argument("--json", action="store_true", help="Emit machine-readable JSON")
    info.set_defaults(func=cmd_info)

    doctor = sub.add_parser("doctor", help="Run Cascade diagnostics")
    doctor_sub = doctor.add_subparsers(dest="doctor_command", required=True)
    environment = doctor_sub.add_parser("env", help="Check the runtime environment and installation paths")
    environment.add_argument("--root-exe", default="root", help="ROOT executable name/path")
    environment.add_argument("--json", action="store_true", help="Emit machine-readable JSON")
    environment.set_defaults(func=cmd_doctor_env)
    plugins = doctor_sub.add_parser("plugins", help="Verify plugin manifests, hashes, signatures, and ABI")
    plugins.add_argument("--cpp-dir", help="C++ plugin directory")
    plugins.add_argument("--py-dir", help="Python plugin directory")
    plugins.add_argument("--trust-store", help="Directory containing trusted plugin public keys")
    plugins.add_argument("--json", action="store_true", help="Emit machine-readable verification results")
    plugins.set_defaults(func=cmd_doctor_plugins)

    plugin = sub.add_parser("plugin", help="Install plugins and manage persistent plugin prefixes")
    plugin_sub = plugin.add_subparsers(dest="plugin_command", required=True)
    plugin_install = plugin_sub.add_parser("install", help="Build, verify, publish, and register a plugin")
    plugin_install.add_argument("source", help="Plugin source directory containing SConstruct")
    plugin_install.add_argument(
        "--prefix",
        default=os.path.expanduser("~/.local"),
        help="Plugin installation prefix (default: ~/.local)",
    )
    plugin_install.add_argument("--package", help="Installed package name (default: source directory name)")
    plugin_install.add_argument("--private-key", help="Ed25519 private key used to sign installed manifests")
    plugin_install.add_argument("--public-key", help="Public key installed into the target trust store")
    plugin_install.add_argument("--jobs", type=_positive_int, default=2, help="Parallel SCons jobs")
    plugin_install.add_argument("--scons", default="scons", help="SCons executable name or path")
    plugin_install.add_argument("--json", action="store_true", help="Emit machine-readable installation result")
    plugin_install.set_defaults(func=cmd_plugin_install)

    plugin_path = plugin_sub.add_parser("path", help="Manage persistent plugin prefixes")
    plugin_path_sub = plugin_path.add_subparsers(dest="plugin_path_command", required=True)
    plugin_path_list = plugin_path_sub.add_parser("list", help="List persistent plugin prefixes")
    plugin_path_list.add_argument("--json", action="store_true", help="Emit machine-readable prefix information")
    plugin_path_list.set_defaults(func=cmd_plugin_path_list)
    plugin_path_add = plugin_path_sub.add_parser("add", help="Register a persistent plugin prefix")
    plugin_path_add.add_argument("prefix", help="Plugin installation prefix")
    plugin_path_add.add_argument("--create", action="store_true", help="Create the prefix if it does not exist")
    plugin_path_add.add_argument("--json", action="store_true", help="Emit machine-readable result")
    plugin_path_add.set_defaults(func=cmd_plugin_path_add)
    plugin_path_remove = plugin_path_sub.add_parser("remove", help="Unregister a persistent plugin prefix")
    plugin_path_remove.add_argument("prefix", help="Plugin installation prefix")
    plugin_path_remove.add_argument("--json", action="store_true", help="Emit machine-readable result")
    plugin_path_remove.set_defaults(func=cmd_plugin_path_remove)

    module = sub.add_parser("module", help="Discover or run verified analysis modules")
    module_sub = module.add_subparsers(dest="module_command", required=True)
    module_list = module_sub.add_parser("list", help="List available C++ and Python modules")
    module_list.add_argument("--language", choices=("cpp", "python"), help="Filter by implementation language")
    module_list.add_argument("--tag", help="Filter by metadata tag")
    module_list.add_argument("--json", action="store_true", help="Emit machine-readable JSON")
    module_list.set_defaults(func=cmd_module_list)
    module_run = module_sub.add_parser("run", help="Register and run one verified module")
    module_run.add_argument("module", help="Registered module class name")
    module_run.add_argument("--name", help="Instance name")
    module_run.add_argument("--params", help="YAML or JSON parameter file")
    module_run.add_argument("--set", action="append", default=[], metavar="KEY=VALUE", help="Override a registered parameter")
    module_run.add_argument("--output-directory", help="Module output root")
    module_run.add_argument("--cache-directory", help="Module cache root")
    module_run.add_argument("--isolated", action="store_true", help="Run the module in a subprocess")
    module_run.add_argument("--json", action="store_true", help="Emit machine-readable result JSON")
    module_run.set_defaults(func=cmd_module_run)

    cache = sub.add_parser("cache", help="Inspect and prune snapshot cache entries")
    cache_sub = cache.add_subparsers(dest="cache_command", required=True)
    cache_list = cache_sub.add_parser("list", help="List cached snapshot hashes")
    cache_list.add_argument("--cache-directory", help="Snapshot cache root")
    cache_list.add_argument("--module", help="Filter by module instance name")
    cache_list.add_argument("--json", action="store_true", help="Emit machine-readable JSON")
    cache_list.set_defaults(func=cmd_cache_list)
    cache_explain = cache_sub.add_parser("explain", help="Explain a snapshot cache hit or miss")
    cache_explain.add_argument("module", help="Module instance name")
    cache_explain.add_argument("hash", help="Snapshot hash")
    cache_explain.add_argument("--cache-directory", help="Snapshot cache root")
    cache_explain.add_argument("--json", action="store_true", help="Emit machine-readable JSON")
    cache_explain.set_defaults(func=cmd_cache_explain)
    cache_prune = cache_sub.add_parser("prune", help="Remove unusable snapshot entries")
    cache_prune.add_argument("--cache-directory", help="Snapshot cache root")
    cache_prune.add_argument("--module", help="Restrict pruning to one module instance")
    cache_prune.add_argument(
        "--all", action="store_true", help="Remove every matching snapshot instead of only entries with missing provenance"
    )
    cache_prune.add_argument("--dry-run", action="store_true", help="Show what would be removed")
    cache_prune.add_argument("--json", action="store_true", help="Emit machine-readable JSON")
    cache_prune.set_defaults(func=cmd_cache_prune)

    dag = sub.add_parser("dag", help="Run declarative mixed-language DAG workflows")
    dag_sub = dag.add_subparsers(dest="dag_command", required=True)
    dag_run = dag_sub.add_parser("run", help="Run a schema-versioned DAG workflow")
    dag_run.add_argument("workflow", help="Workflow YAML or JSON file")
    fail_fast = dag_run.add_mutually_exclusive_group()
    fail_fast.add_argument("--fail-fast", dest="fail_fast", action="store_true", help="Stop after the first failed branch")
    fail_fast.add_argument("--keep-going", dest="fail_fast", action="store_false", help="Finish independent branches")
    dag_run.add_argument("--dot", help="Write final DAG state to this DOT file")
    dag_run.add_argument("--provenance", help="Write workflow provenance to this JSON file")
    dag_run.add_argument("--json", action="store_true", help="Emit machine-readable result JSON")
    dag_run.set_defaults(func=cmd_dag_run, fail_fast=None)
    dag_validate = dag_sub.add_parser("validate", help="Validate a workflow without executing modules")
    dag_validate.add_argument("workflow", help="Workflow YAML or JSON file")
    dag_validate.add_argument("--json", action="store_true", help="Emit machine-readable validation result")
    dag_validate.set_defaults(func=cmd_dag_validate)

    history = sub.add_parser("history", help="List module and workflow runs from provenance")
    history.add_argument("--root", action="append", help="Search this provenance file or directory (repeatable)")
    history.add_argument("--kind", choices=("all", "module", "workflow"), default="all", help="Filter run type")
    history.add_argument("--limit", type=_positive_int, default=20, help="Maximum runs to show")
    history.add_argument("--json", action="store_true", help="Emit machine-readable JSON")
    history.set_defaults(func=cmd_history)

    inspect_run = sub.add_parser("inspect", help="Inspect a module or workflow run")
    inspect_run.add_argument("run", help="Run ID or provenance manifest path")
    inspect_run.add_argument("--root", action="append", help="Search this provenance file or directory (repeatable)")
    inspect_run.add_argument("--json", action="store_true", help="Emit the complete provenance manifest")
    inspect_run.set_defaults(func=cmd_inspect)

    diff = sub.add_parser("diff", help="Compare reproducibility-relevant run state")
    diff.add_argument("before", help="Earlier run ID or provenance manifest path")
    diff.add_argument("after", help="Later run ID or provenance manifest path")
    diff.add_argument("--root", action="append", help="Search this provenance file or directory (repeatable)")
    diff.add_argument("--json", action="store_true", help="Emit machine-readable JSON")
    diff.set_defaults(func=cmd_diff)

    replay = sub.add_parser("replay", help="Replay a module run from its provenance")
    replay.add_argument("run", help="Run ID or module provenance manifest path")
    replay.add_argument("--root", action="append", help="Search this provenance file or directory (repeatable)")
    replay.add_argument("--name", help="Override the module instance name")
    replay.add_argument("--set", action="append", default=[], metavar="KEY=VALUE", help="Override a recorded parameter")
    replay.add_argument("--output-directory", help="Override the recorded output root")
    replay.add_argument("--cache-directory", help="Override the recorded cache root")
    replay_isolation = replay.add_mutually_exclusive_group()
    replay_isolation.add_argument("--isolated", dest="isolated", action="store_true", help="Run in a subprocess")
    replay_isolation.add_argument("--no-isolated", dest="isolated", action="store_false", help="Run in the current process")
    replay.add_argument("--json", action="store_true", help="Emit machine-readable result JSON")
    replay.set_defaults(func=cmd_replay, isolated=None)

    macro = sub.add_parser("macro", help="Run a legacy ROOT macro")
    macro_sub = macro.add_subparsers(dest="macro_command", required=True)
    macro_run = macro_sub.add_parser("run", help="Run a ROOT macro with parameter injection")
    macro_run.add_argument("macro", help="Path to ROOT macro")
    macro_run.add_argument("--yaml", help="YAML config path, passed as _yaml_path")
    macro_run.add_argument("--set", action="append", default=[], metavar="KEY=VALUE", help="Add a temporary JSON parameter")
    macro_run.add_argument("--extra", action="append", default=[], help="Add a string macro argument")
    macro_run.add_argument("--root-exe", default="root", help="ROOT executable name/path")
    macro_run.add_argument("--no-plus", action="store_true", help="Do not append '+' to the macro path")
    macro_run.set_defaults(func=cmd_macro_run)

    legacy = p.add_argument_group("legacy ROOT macro compatibility")
    legacy.add_argument("--macro", help="Path to ROOT macro (prefer: cascade macro run MACRO)")
    legacy.add_argument("--yaml", help="YAML config path (packed into JSON as _yaml_path)")
    legacy.add_argument("--set", action="append", metavar="KEY=VALUE")
    legacy.add_argument("--extra", action="append", default=[], help="extra string args to macro")
    legacy.add_argument("--root-exe", default="root", help="ROOT executable name/path")
    legacy.add_argument("--no-plus", action="store_true", help="Do not append '+' to macro")
    p.set_defaults(func=cmd_macro_run)
    return p
