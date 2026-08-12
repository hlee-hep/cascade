"""Cascade public Python API."""

import importlib

from ._cascade import (
    CacheManager,
    CancellationToken,
    ExecutionContext,
    IAnalysisModule,
    ModulePhase,
    ModuleRunManifest,
    ModuleStatus,
    OutputTransaction,
    ParamManager,
    PluginPaths,
    PluginVerifier,
    ProvenanceRecorder,
    RunResult,
    RuntimeOptions,
    SnapshotHasher,
    configure_runtime,
    get_abi_tag,
    get_abi_version,
    get_runtime_options,
    get_version,
    init_interrupt,
    is_interrupted,
    log,
    log_level,
    set_log_file,
    set_log_level,
    set_runtime_options,
)

__version__ = get_version()
__abi_version__ = get_abi_version()
__abi_tag__ = get_abi_tag()

_LAZY_MODULES = {
    "Controller": "py_amcm",
    "plt_plot_manager": "plt_plot_manager",
    "plugin_paths": "plugin_paths",
}

__all__ = [
    "Controller",
    "log_level",
    "CacheManager",
    "CancellationToken",
    "ExecutionContext",
    "IAnalysisModule",
    "ModulePhase",
    "ModuleStatus",
    "OutputTransaction",
    "ParamManager",
    "PluginPaths",
    "ModuleRunManifest",
    "PluginVerifier",
    "ProvenanceRecorder",
    "RunResult",
    "RuntimeOptions",
    "SnapshotHasher",
    "configure_runtime",
    "get_runtime_options",
    "set_runtime_options",
    "set_log_level",
    "set_log_file",
    "get_version",
    "get_abi_version",
    "get_abi_tag",
    "__version__",
    "__abi_version__",
    "__abi_tag__",
    "init_interrupt",
    "is_interrupted",
    "log",
] + [name for name in _LAZY_MODULES if name != "Controller"]


def __getattr__(name):
    if name in _LAZY_MODULES:
        module = importlib.import_module(f".{_LAZY_MODULES[name]}", __name__)
        value = getattr(module, name)
        globals()[name] = value
        return value
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
