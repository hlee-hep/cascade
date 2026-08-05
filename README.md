<p align="center">
  <img src="docs/framework.png" alt="Cascade" width="300"/>
  <img src="docs/framework_light.png" alt="Cascade light" width="300"/>
</p>

# Cascade

Cascade is a C++/Python analysis framework for ROOT-based workflows. Analysis
code is packaged as verified plugins with optional publisher signing; the core
supplies lifecycle management,
typed parameters, ROOT I/O, DAG execution, reproducible caching, transactional
outputs, versioned provenance, and optional subprocess isolation.

The current development release is **0.3.0** with
**plugin ABI 3**. The full build fingerprint is checked in addition to the
integer ABI.

Release notes are tracked in [CHANGELOG.md](CHANGELOG.md), and the verification
process is documented in [Preparing a release](docs/releasing.md).
Cascade is distributed under the [MIT License](LICENSE).

## Why Cascade

A Cascade analysis module concentrates on analysis logic while the framework
owns the operational boundary:

- `Init → Check → Execute → Finalize → Commit` lifecycle with phase-aware errors;
- the same module controller for C++ and Python plugins;
- typed parameters with YAML/JSON/Python round trips;
- classic `TTree` and `RDataFrame` analysis paths;
- schema-validated input, cut, and histogram configuration;
- deterministic snapshot caching linked to versioned provenance manifests;
- transactional output promotion and rollback;
- DAG dependencies and parameter links;
- verified plugin manifests, optional signatures, and strict C++ build fingerprints;
- subprocess isolation for native crashes and abnormal exits.

## Core model

| Component | Responsibility |
| --- | --- |
| `IAnalysisModule` / `base_module` | C++/Python module lifecycle and framework contract |
| `ExecutionContext` | Run ID, output/cache roots, cancellation, logging, output transaction |
| `AnalysisManager` | ROOT inputs, branches, cuts, histograms, metadata, RDF |
| `ParamManager` | Registered typed parameters and YAML/JSON serialization |
| `DAGManager` | Stateful dependency execution, failure propagation, and generic data links |
| `PlotManager` | ROOT stack, overlay, ratio, legend, and style helpers |
| `AMCM` / `py_amcm` | Registration, execution, progress, provenance, isolation |

The supported Python control surface is `py_amcm`. `cascade._cascade` and the raw
`AMCM` binding are internal integration surfaces.

## Build, test, and install

### Requirements

- Linux;
- ROOT with `root-config` available;
- a compiler supporting the C++ standard reported by `root-config` (C++17, 20, or 23);
- Python 3 with pybind11;
- PyYAML;
- SCons;
- yaml-cpp;
- OpenSSL;
- nlohmann/json headers.

Matplotlib is optional for `plt_plot_manager`; PyROOT is optional for Python code
that reads ROOT objects directly.

Make sure the dependency probes succeed:

```bash
root-config --version
pkg-config --modversion yaml-cpp
python3 -m pybind11 --includes
scons --version
openssl version
```

Then build and run the complete test suite:

```bash
scons -j2
scons verify -j2
scons install PREFIX=/your/cascade/prefix
```

The default prefix is `~/.local`. See [Build and installation](docs/build.md) for
all install variables and runtime environment setup.

## First complete run

The repository includes a verified mixed-language package with four modules:

```text
TextProducerModule (C++) ──> TextTransformModule (Python)
RootEventModule (C++) ─────> RootSummaryModule (Python)
```

Install the example into a persistent plugin prefix:

```bash
cascade plugin install examples/plugins/mixed_pipeline \
  --prefix ~/.local
```

The command builds into a staging prefix, verifies both languages, publishes the
package, and records the prefix in the user Cascade configuration. Future
terminals discover it without `CASCADE_PLUGIN_DIR` or `CASCADE_PYPLUGIN_DIR`.

Verify the installed package and run both execution modes:

```bash
cascade doctor plugins
cascade doctor runtime
cascade module list
cascade dag run workflow.yaml
python3 run_pipeline.py --output example-output
python3 run_pipeline.py --isolated --output isolated-output
```

The output directories contain:

```text
events.root
events_manifest.json
events_summary.json
message.json
message_upper.json
mixed_pipeline.dot
```

See [Quickstart](docs/quickstart.md) for environment setup, expected status
messages, and common first-run failures.

## Writing a module

### C++

```cpp
#include "IAnalysisModule.hh"

#include <fstream>

class EventCountModule final : public IAnalysisModule
{
  public:
    EventCountModule()
    {
        SetBaseName("EventCountModule");
        SetCodeHash("replace-at-build-time");
        Parameters().Register<std::string>("output", "count.txt");
        Parameters().Register<int>("count", 10);
    }

    void Description() const override {}

  protected:
    void Init() override
    {
        if (Parameters().Get<int>("count") < 0) throw std::invalid_argument("count must be non-negative");
    }

    void Execute() override
    {
        std::ofstream output(StageOutput(Parameters().Get<std::string>("output")));
        output << Parameters().Get<int>("count") << '\n';
        if (!output) throw std::runtime_error("cannot write output");
    }

    void Finalize() override {}
};
```

### Python

```python
from cascade.pymodule.base_module import base_module


class EventCountPythonModule(base_module):
    SUMMARY = "Writes a configured event count."
    TAGS = ["example"]

    def __init__(self):
        super().__init__()
        self.basename = "EventCountPythonModule"
        self.code_version_hash = "replace-at-build-time"
        self.register_param("output", "count.json")
        self.register_param("count", 10)

    def print_description(self):
        print(self.SUMMARY)

    def init(self):
        if self.get_param("count") < 0:
            raise ValueError("count must be non-negative")

    def execute(self):
        self.stage_output(self.get_param("output")).write_text(
            f'{{"count": {self.get_param("count")}}}\n',
            encoding="utf-8",
        )

    def finalize(self):
        pass
```

Important contracts:

- register every externally configurable parameter before assignment;
- validate cheap configuration in `Init`;
- put analysis work in `Execute`;
- write protected outputs only through `StageOutput`/`stage_output`;
- periodically check cancellation in long loops;
- do not depend on child-only object mutations after isolated execution.

The complete authoring guide is [Writing analysis modules](docs/module-authoring.md).
The manager-specific references are
[Parameters](docs/parameters.md),
[AnalysisManager](docs/analysis-manager.md),
[DAG execution](docs/dag.md), and
[Plotting](docs/plotting.md).

## Configuration

AnalysisManager configuration uses schema version 1:

```yaml
schema_version: 1
input:
  files: [events.root]
  tree: events
branches:
  pt:
    name: jet_pt
    type: Float_t
```

`LoadInputConfig`, `LoadCutConfig`, and `LoadHistogramConfig` run preflight
automatically. Their `Preflight*Config` counterparts collect errors without
mutating manager state.

See [Configuration schema](docs/configuration.md) for complete input, cut, and
histogram examples and supported branch types.

## Execution results

Every run returns a `RunResult`:

| Field | Meaning |
| --- | --- |
| `status` / `Status` | `Done`, `Skipped`, `Interrupted`, or `Failed` |
| `phase` / `Phase` | Lifecycle phase responsible for the result |
| `message` / `Message` | Human-readable diagnostic or skip reason |
| `exception` / `Exception` | Python exception or C++ `std::exception_ptr` |

`Skipped` is expected for `dry_run` or a cached snapshot. It is not a failure.
`Interrupted` rolls back staged outputs. A `Failed` commit also restores previously
existing output files.

For native-risk modules:

```python
result = controller.run_module_isolated("module_instance")
```

Isolation preserves committed files and cache state, but not changes made only to
the worker module object's memory. It is Linux-only: worker hardening and descriptor
handling use Linux `prctl` and `/proc` interfaces in addition to process spawning.

See [Execution contract](docs/execution.md).

## Plugin verification and compatibility

Installed plugins are verified by default when:

1. the package manifest uses schema 2;
2. every listed file matches its SHA-256 digest;
3. module names and paths satisfy package rules;
4. C++ ABI version and full build fingerprint match the runtime.

Publisher signatures are optional. Use `cascade --require-signed ...` when every
plugin must also have an Ed25519 signature matching the external trust store.

Additional plugin prefixes are persisted with `cascade plugin install` or
`cascade plugin path add`; each new process rescans and revalidates them.

The ABI fingerprint includes compiler, standard library, C++ mode, ROOT version,
pointer width, build mode, and libstdc++ ABI/debug settings.

```bash
cascade doctor plugins
```

See [Plugin development and distribution](docs/plugins.md) and
[Migrating to 0.3](docs/migration-0.3.md).

## Documentation

| Guide | Use it when |
| --- | --- |
| [Quickstart](docs/quickstart.md) | Building and running the included mixed plugin for the first time |
| [Build and installation](docs/build.md) | Configuring prefixes, libraries, Python paths, and local verification |
| [Writing analysis modules](docs/module-authoring.md) | Implementing C++ or Python analysis logic |
| [Parameters](docs/parameters.md) | Declaring typed contracts and loading YAML/JSON values |
| [Configuration schema](docs/configuration.md) | Authoring input, cut, histogram, and parameter files |
| [AnalysisManager](docs/analysis-manager.md) | Building classic TTree and RDataFrame analyses |
| [Execution contract](docs/execution.md) | Understanding lifecycle, cache, transactions, cancellation, isolation |
| [Runtime reliability and performance](docs/runtime-reference.md) | Choosing hashing, cache, DAG, worker, and isolation settings |
| [DAG execution](docs/dag.md) | Composing modules and propagating failures or parameters |
| [Command-line interface](docs/cli.md) | Diagnosing installations and running modules, DAG workflows, or ROOT macros |
| [Plotting](docs/plotting.md) | Producing ROOT or Matplotlib plots |
| [Plugin guide](docs/plugins.md) | Packaging, verifying, optionally signing, and diagnosing plugins |
| [Examples](docs/examples.md) | Finding runnable scripts and ROOT macros |
| [Migration to 0.3](docs/migration-0.3.md) | Updating pre-0.3 source and older configuration |
| [Troubleshooting](docs/troubleshooting.md) | Resolving build, plugin, cache, config, and runtime failures |
| [Versioning](docs/versioning.md) | Semantic version and ABI policy |
| [Architecture](docs/architecture.md) | Understanding component relationships and ownership |
| [Coding conventions](docs/CONVENTIONS.md) | Contributing framework code |

## Operational boundaries

- Files written directly to final paths are outside the output transaction.
- Subprocess isolation contains plugin crashes; it is not a security sandbox.
- Snapshot caching assumes a module's parameters, manager state, code hash, and
  output root describe its deterministic inputs.
- Python plugin discovery accepts only verified manifest entries ending in
  `module.py`.
- C++ plugins must be rebuilt whenever the plugin ABI or fingerprint changes;
  signed distributions must also be re-signed.

## Development checks

```bash
scons verify -j2
scons compdb
scons tidy
```

`verify` runs the complete test suite, compiles the public plugin boundary without
ROOT headers, checks working-tree whitespace, and runs runtime/plugin diagnostics.

The `tidy` target requires `clang-tidy` to be installed and available on
`PATH`.
