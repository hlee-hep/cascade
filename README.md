<p align="center">
  <img src="docs/framework.png" alt="Cascade Logo" width="300"/>
  <img src="docs/framework_light.png" alt="Cascade Logo Light" width="300"/>
</p>

<br>

# Cascade

Cascade is a modular analysis framework for HEP workflows. It combines ROOT-based I/O, a typed parameter system, a module registry, DAG orchestration, and both C++ and Python frontends to make analyses reproducible and portable.

## Overview

Cascade is built around a small set of managers and a module interface:

- **AnalysisManager** controls ROOT input/output, `TChain`/`TTree` access, cut evaluation, histogram booking/filling, and `RDataFrame` workflows.
- **ParamManager** handles typed parameters from YAML/JSON and Python, with serialization and mixed-type vectors.
- **PlotManager** provides ROOT plotting utilities (stack, overlays, ratio pad, legend automation).
- **DAGManager** executes dependency graphs and supports parameter passing between nodes.
- **AMCM** is the core C++ controller used by the Python wrapper; it is intentionally not re-exported at the top-level Python API.
- **Logger** provides structured logging with colorized output and progress bars.

Each analysis is a module that inherits `IAnalysisModule` and defines `Init`, `Execute`, and `Finalize`. The framework supplies shared services (config, parameters, logging, DAG, caching) so modules can focus on physics logic.

## Key Features

- ROOT I/O with both classic `TTree` loops and `RDataFrame` pipelines.
- YAML/JSON driven configuration and parameter injection.
- Module registry with auto-registration for plugins.
- Python API via pybind11 for scripting and control.
- DAG execution with automatic parameter propagation between nodes.
- Snapshot hashing to detect duplicated runs.

## External Dependencies

- [ROOT](https://root.cern/) — I/O, `TTree`, `RDataFrame`, plotting
- [pybind11](https://github.com/pybind/pybind11) — C++/Python bindings
- [yaml-cpp](https://github.com/jbeder/yaml-cpp) — YAML config parsing
- [nlohmann/json](https://github.com/nlohmann/json) — JSON serialization (header-only)
- [openSSL](https://openssl-library.org) — SHA256 hashing
- [SCons](https://scons.org/) — build system (`pip install scons`)
- C++17 or higher

## Build and Install

Default install prefix is `~/.local`.

```bash
scons
scons install
```

You can override install locations via SCons variables:

```bash
scons PREFIX=/opt/cascade \
      LIBDIR=/opt/cascade/lib \
      BINDIR=/opt/cascade/bin \
      INCLUDEDIR=/opt/cascade/include/cascade \
      PYTHONDIR=/opt/cascade/python \
      PYMODULEDIR=/opt/cascade/python/pymodule
```

Notes:
- `PREFIX` controls defaults for the other paths.
- If `LIBDIR`, `BINDIR`, `INCLUDEDIR`, `PYTHONDIR`, or `PYMODULEDIR` are not set, they are derived from `PREFIX`.

## Core Flow

1. Register a module (C++ or Python).
2. Load parameters (YAML/JSON/Python dict).
3. Initialize inputs (ROOT files, trees, aliases).
4. Run (`Init` -> `Execute` -> `Finalize`).
5. Save outputs (trees, histograms, metadata).

## Module API (C++)

Modules inherit from `IAnalysisModule` and override:

- `Init()` — set up managers, inputs, cuts.
- `Execute()` — perform event loop or RDF pipeline.
- `Finalize()` — write outputs and metadata.

Example:

```cpp
class MyModule : public IAnalysisModule {
public:
  void Init() override {
    auto *mgr = GetAnalysisManager("main");
    mgr->LoadInputConfig("config.yaml");
    mgr->BuildChain();
  }

  void Execute() override {
    auto *mgr = GetAnalysisManager("main");
    for (Long64_t i = 0; i < mgr->GetEntryCount(); ++i) {
      mgr->LoadEvent(i);
      if (!mgr->PassesAllCuts()) continue;
      mgr->FillHistograms(1.0);
    }
  }

  void Finalize() override {
    auto *mgr = GetAnalysisManager("main");
    mgr->WriteHistograms("hists.root");
  }
};
```

## Python API

Python modules live in `modules/python/` and inherit `base_module`.

Core Python control entry points:

- `py_amcm` — wrapper around the C++ controller (run modules, DAG).
- `plt_plot_manager` — matplotlib-based plotting utility.

Public API boundary:

- `py_amcm` is the supported Python control surface.
- `cascade._cascade` is internal; use it only if you need direct C++ bindings.
- `AMCM` is intentionally not re-exported at top level to keep the public API small.

The CLI `python/cascade` is a minimal ROOT macro wrapper that injects `--yaml` and `--set` into a temporary JSON file passed as the first macro argument.

An example ROOT macro is provided in `examples/RootMacroExample.C`.
An example plugin module is provided in `examples/PluginExample.cc`.

Run logs from `py_amcm.save_run_log_all()` are written under `~/.cache/cascade/run_logs/` by default. You can override the output directory with `CASCADE_RUN_LOG_DIR` or by passing `log_dir=...`.

Versioning:

- `cascade.__version__` exposes the semantic version string.
- `cascade.__abi_version__` exposes the plugin ABI version.

Module metadata:

- `ctrl.get_list_available_module_metadata()` returns per-module metadata (name, version, summary, tags).
- C++ modules can register metadata via `REGISTER_MODULE_WITH_METADATA`.
- Python modules can provide a `METADATA` dict or class attributes (`VERSION`, `SUMMARY`, `TAGS`) without instantiation.

## Build and Install Layout

SCons installs to `${PREFIX}` with per-component overrides:

- Libraries: `${LIBDIR}` (default: `${PREFIX}/lib`)
- Headers: `${INCLUDEDIR}` (default: `${PREFIX}/include/cascade`)
- CLI: `${BINDIR}` (default: `${PREFIX}/bin`)
- Python package: `${PYTHONDIR}` (default: `${LIBDIR}/cascade`)
- Python modules: `${PYMODULEDIR}` (default: `${PYTHONDIR}/pymodule`)

The pybind library `libCascade.so` is installed to `${LIBDIR}` and symlinked into `${PYTHONDIR}` as `_cascade.so`.
See `docs/build.md` for a detailed build/install flow and output layout.

## Plugin ABI

External C++ plugins should export two C symbols so the loader can check ABI compatibility and register modules:

```cpp
#include "PluginABI.hh"
#include "MyModule.hh"

CASCADE_PLUGIN_EXPORT int CascadePluginAbiVersion() { return CASCADE_PLUGIN_ABI_VERSION; }
CASCADE_PLUGIN_EXPORT void CascadeRegisterPlugin() { CASCADE_REGISTER_MODULE(MyModule); }
```

Plugins without these entry points are still loaded but will emit a warning and skip ABI checks.

See `docs/plugins.md` for the ABI policy and plugin developer guide.
See `docs/versioning.md` for versioning policy and API.
See `docs/build.md` for a detailed build/install flow and output layout.
See `docs/troubleshooting.md` for common issues and fixes.
See `docs/faq.md` for short example answers.
See `docs/examples.md` for runnable examples.
See `docs/quickstart.md` for an end-to-end walkthrough.

Plugin loading notes:

- Only shared libraries ending with `Module.so` are loaded from the plugin directory.
- Plugins require a `plugin_pubkey.pem` in the plugin directory; otherwise they are skipped.
- When the key exists, plugins must provide a valid `.sig` signature.
- The signing helper installs to `${PREFIX}/share/cascade/scripts/sign_plugin.sh`.
- Python plugins apply the same signature rule when a `plugin_pubkey.pem` exists in their directory.

## Configuration

Inputs and cuts are YAML-driven:

- Input config defines files, trees, and branch aliases.
- Cut config defines named expressions.
- Histogram config defines expressions and bins.

These configs can be generated and written by `AnalysisManager` as part of your pipeline.
