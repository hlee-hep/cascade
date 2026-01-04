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
- **AMCM** registers and executes modules, tracks progress, and coordinates runs.
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

Python modules live in `modules/python/` and inherit `py_base_module`.

Core Python control entry points:

- `py_amcm` — wrapper around the C++ controller (run modules, DAG).
- `plt_plot_manager` — matplotlib-based plotting utility.

The CLI `python/cascade` can launch an interactive shell with `ctrl = py_amcm()`.

## Configuration

Inputs and cuts are YAML-driven:

- Input config defines files, trees, and branch aliases.
- Cut config defines named expressions.
- Histogram config defines expressions and bins.

These configs can be generated and written by `AnalysisManager` as part of your pipeline.
