# Examples

## Toy dimuon resonance analysis

Location: `examples/plugins/toy_dimuon_analysis`

This is the complete physics-analysis example:

```text
toy event generation (C++/ROOT)
  -> RDataFrame selection (C++/ROOT)
  -> Voigt + exponential mass fit (C++/ROOT)
  -> Markdown/JSON report (ROOT-free Python)
```

It needs no external data and demonstrates deterministic generation, typed
parameters, a four-node DAG, transactional multi-file output, cache reuse,
provenance, worker isolation, a cutflow, and a fit pull plot.

```bash
cd examples/plugins/toy_dimuon_analysis
cascade plugin install . --prefix ~/.local
python3 run_analysis.py --output example-output
python3 run_analysis.py --output example-output  # cache reuse
```

Use `--float-width` to float the resonance width and `--isolated` to exercise
the worker boundary. See the package
[README](../examples/plugins/toy_dimuon_analysis/README.md) for the fit model,
outputs, and declarative workflow.

## Mixed plugin package

Location: `examples/plugins/mixed_pipeline`

This is the primary end-to-end example. It contains:

| Module | Language | Input | Output |
| --- | --- | --- | --- |
| `TextProducerModule` | C++ | Parameters | `message.json` |
| `TextTransformModule` | Python | `message.json` | `message_upper.json` |
| `RootEventModule` | C++/ROOT | Parameters | `events.root`, `events_manifest.json` |
| `RootSummaryModule` | Python | ROOT file or manifest | `events_summary.json` |

It demonstrates:

- C++ and Python modules in one verified package;
- generated code-version hashes;
- transactional multi-file output;
- a two-branch DAG;
- optional PyROOT use;
- subprocess isolation;
- plugin manifest and ABI diagnostics.

Build/install:

```bash
cd examples/plugins/mixed_pipeline
cascade plugin install . --prefix ~/.local
```

Run:

```bash
cascade doctor plugins
cascade module list
cascade dag run workflow.yaml
python3 run_pipeline.py --output example-output
python3 run_pipeline.py --isolated --output isolated-output
```

See the package [README](../examples/plugins/mixed_pipeline/README.md).

## Minimal controller example

Location: `examples/QuickstartExample.py`

Registers `TextProducerModule`, configures output/cache roots and parameters, runs
it, then prints status and progress.

```bash
python3 examples/QuickstartExample.py
```

Requires the mixed plugin package above.

## Mixed-language DAG example

Location: `examples/DAGPluginExample.py`

Creates:

```text
producer (C++) -> transform (Python)
```

```bash
python3 examples/DAGPluginExample.py
```

The script writes output and `dag_plugin_example.dot` under
`dag-plugin-output/`.

Render the graph:

```bash
dot -Tpng dag-plugin-output/dag_plugin_example.dot -o dag-plugin-output/dag_plugin_example.png
```

See [DAG execution](dag.md) for failure propagation, reset behavior,
parameter links, and execution limits.

## ROOT macro CLI example

Location: `examples/RootMacroExample.C`

The `cascade` CLI converts `--yaml` and `--set` arguments to a temporary JSON
document passed as the first ROOT macro argument.

```bash
cascade macro run examples/RootMacroExample.C \
  --set n=1000 \
  --set mode='"fast"'
```

The legacy `cascade --macro ...` spelling remains available. Use this adapter
for existing ROOT macros. New plugin workflows can use `cascade dag run` or
`py_amcm`.

## Direct manager example

Location: `examples/RootManagersExample.C`

Demonstrates:

- direct `ParamManager` registration and JSON output;
- generation of an input schema with `WriteInputConfig`;
- a basic `PlotManager` overlay.

Run in an environment where Cascade ROOT dictionaries are discoverable:

```bash
root -l 'examples/RootManagersExample.C()'
```

See [AnalysisManager](analysis-manager.md), [Parameters](parameters.md), and
[Plotting](plotting.md) for the complete manager contracts.

## Choosing an example

| Goal | Start with |
| --- | --- |
| Verify installation | `QuickstartExample.py` |
| Follow a complete physics analysis | `plugins/toy_dimuon_analysis` |
| Learn fitting, cutflows, and cache reuse | `plugins/toy_dimuon_analysis` |
| Learn plugin structure | `plugins/mixed_pipeline` |
| Learn DAG callbacks | `DAGPluginExample.py` |
| Integrate a legacy ROOT macro | `RootMacroExample.C` |
| Use managers directly in ROOT | `RootManagersExample.C` |
