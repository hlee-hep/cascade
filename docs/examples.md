# Examples

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

- C++ and Python modules in one signed distribution;
- generated code-version hashes;
- transactional multi-file output;
- a two-branch DAG;
- optional PyROOT use;
- subprocess isolation;
- plugin manifest and ABI diagnostics.

Build/install:

```bash
cd examples/plugins/mixed_pipeline
openssl genpkey -algorithm Ed25519 -out plugin_private.pem
openssl pkey -in plugin_private.pem -pubout -out plugin_public.pem

CASCADE_PLUGIN_PACKAGE=mixed_pipeline \
CASCADE_PLUGIN_PRIVATE_KEY="$PWD/plugin_private.pem" \
CASCADE_PLUGIN_PUBLIC_KEY="$PWD/plugin_public.pem" \
scons install
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
| Learn plugin structure | `plugins/mixed_pipeline` |
| Learn DAG callbacks | `DAGPluginExample.py` |
| Integrate a legacy ROOT macro | `RootMacroExample.C` |
| Use managers directly in ROOT | `RootManagersExample.C` |
