<p align="center">
  <img src="docs/framework.png" alt="Cascade Logo" width="300"/>
  <img src="docs/framework_light.png" alt="Cascade Logo Light" width="300"/>
</p>

# Cascade

**Modular HEP analysis framework** with support for **ROOT**, **RDataFrame**, **DAG-based execution**, and **snapshot tracking**.

> 🚧 **Cascade is under active development — stay tuned!**

---

## 📁 Directory Structure

- `include/`, `src/` — Core C++ framework components
- `modules/` — Individual analysis modules (plug-and-play)
- `AnalysisManager/` — Manages ROOT I/O and analysis flow
- `ParamManager/` — Handles parameter input and lambda registration
- `PlotManager/` — Centralized histogram and plot rendering
- `python/` — Python wrapper and control interface
- `main/` — Python binding entry point (`pybind11`)
- `utils/` — Shared utilities and tools

---

## ⚙️ Build Instructions

Cascade is built using `scons`. To build the framework:

```bash
scons -jX
```

---

## 📦 External Dependencies

- [ROOT](https://root.cern/)
- [pybind11](https://github.com/pybind/pybind11)
- [yaml-cpp](https://github.com/jbeder/yaml-cpp)

---

## 🧠 Core Managers

### 🔧 AnalysisManager
Handles ROOT I/O, TTree, and RDataFrame-based analysis logic.

- Owns and executes ROOT RDataFrame (RDF) and TTree
- Manages cut definitions, new variables, and snapshot outputs
- **Can be used standalone within the ROOT framework (no Python or Cascade required)**

### 📊 PlotManager
Manages ROOT histograms and plotting utilities.

- Creates and renders 1D/2D histograms and graphs
- Designed to be decoupled from analysis logic
- **Can be used independently in ROOT macros similar to AnalysisManager**

### 🧠 ParamManager
Stores and manages parameters within each analysis module.

- Receives Python-side input and provides typed parameter access
- Holds reusable lambda functions for RDF operations
- Simplifies configuration parsing inside each module

### 🧮 DAGManager
Controls directed acyclic graph (DAG) execution for module dependencies.

- Resolves run order based on declared dependencies
- Propagates parameters automatically between connected modules
- Used internally by AMCM for DAG-based orchestration

### 📢 Logger
Unified logging utility used by all managers and modules.

- Provides per-module logging and global messages
- Includes minimal CLI progress bar
- Does not manage any internal progress state

### 🧩 AMCM (AnalysisModuleControlMaster)
C++-side orchestrator that manages analysis module registration and execution.

- Handles module execution and dependency resolution
- Exposed to Python via `pybind11`
- **Wrapped again in Python as `pyAMCM` to support dynamic registration and execution of Python-based modules**

---
