<p align="center">
  <img src="docs/framework.png" alt="Cascade Logo" width="300"/>
  <img src="docs/framework_light.png" alt="Cascade Logo Light" width="300"/>
</p>

<br>

# Cascade

A modular analysis framework for high-energy physics (HEP).
**Cascade** integrates ROOT, C++, and Python into a single, flexible environment for reproducible and maintainable data analysis.

---

## ⚙️ Overview
Cascade provides a modular execution model where each analysis unit (“module”) defines its own logic and parameters.
The framework manages configuration, dependencies, and execution order through a directed acyclic graph (DAG).

---

## 🧩 Core Components
- **AnalysisManager** — handles ROOT I/O, `RDataFrame`, histogramming, and variable definitions.
- **PlotManager** — provides plotting and style control based on `TH1` objects.
- **ParamManager** — type-safe parameter handler with Python-style access (`param["x"]`, `param.Get<float>("x")`).
  The above three managers can also be used independently within ROOT macros.
- **DAGManager** — manages dependencies and execution flow between modules.
- **AMCM** — orchestrates module registration, dependency resolution, and execution monitoring.
- **Logger** — provides unified logging and command-line progress display.
- **SnapshotCacheManager** — handles snapshot hashing and caching for reproducible outputs.

---

## 🧰 Key Features
- Full **C++17 / pybind11** integration
- **RDataFrame**-based event loop
- **YAML**-driven configuration system
- Per-module parameter injection
- **Python** interface for control and scripting
- Core logic implemented in **C++** and **Python**
- Support for user-defined module plugins
- Clean **SCons** build system with ROOT dictionary generation

---

## 📦 External Dependencies

- [ROOT](https://root.cern/) — Data processing, I/O, RDataFrame
- [pybind11](https://github.com/pybind/pybind11) — C++/Python bindings
- [yaml-cpp](https://github.com/jbeder/yaml-cpp) — YAML config parsing
- [nlohmann/json](https://github.com/nlohmann/json) — JSON serialization (single-header)
- [openSSL](https://openssl-library.org) — Hash calculation
- [SCons](https://scons.org/) — Build system (`pip install scons`)
- C++17 or higher

---

## 🔧 Installation

Build and install with SCons. Default install prefix is `~/.local`.

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
- `PREFIX` controls the default values for the other paths.
- If `LIBDIR`, `BINDIR`, `INCLUDEDIR`, `PYTHONDIR`, or `PYMODULEDIR` are not set, they fall back to sensible defaults derived from `PREFIX`.
