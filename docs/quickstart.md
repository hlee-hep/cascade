# Quickstart

This walkthrough builds Cascade, installs the included mixed C++/Python plugin,
verifies its signature and ABI, and runs a two-branch DAG.

## 1. Check dependencies

```bash
root-config --version
pkg-config --modversion yaml-cpp
python3 -m pybind11 --includes
scons --version
openssl version
```

If one command fails, fix that dependency before building. Cascade's SCons files
use these probes directly.

## 2. Build and test Cascade

From the repository root:

```bash
scons -j2
scons test -j2
```

The test target covers the C++ core, Python lifecycle, plugin package discovery,
output rollback, and subprocess crash containment.

## 3. Install into a prefix

Choose one writable prefix and use it consistently:

```bash
scons install PREFIX=/your/cascade/prefix
```

Set the runtime environment:

```bash
export CASCADE_PREFIX=/your/cascade/prefix
export PATH="${CASCADE_PREFIX}/bin:${PATH}"
export PYTHONPATH="${CASCADE_PREFIX}/lib:${PYTHONPATH}"
export LD_LIBRARY_PATH="${CASCADE_PREFIX}/lib:${LD_LIBRARY_PATH}"
```

Verify the Python package:

```bash
python3 -c 'import cascade; print(cascade.__version__, cascade.__abi_version__)'
```

Expected version/ABI for this tree:

```text
0.3.0 1
```

## 4. Create a development signing key

```bash
cd examples/plugins/mixed_pipeline
openssl genpkey -algorithm Ed25519 -out plugin_private.pem
openssl pkey -in plugin_private.pem -pubout -out plugin_public.pem
```

These files are for local development. Do not commit a production private key.

## 5. Install the example plugin

```bash
CASCADE_PLUGIN_PACKAGE=mixed_pipeline \
CASCADE_PLUGIN_PRIVATE_KEY="$PWD/plugin_private.pem" \
CASCADE_PLUGIN_PUBLIC_KEY="$PWD/plugin_public.pem" \
scons install
```

This installs two C++ libraries and two Python modules into separate package
roots, then signs a manifest in each root.

## 6. Verify plugin installation

```bash
cascade doctor plugins
```

A healthy result reports:

- valid manifest signatures;
- matching SHA-256 hashes;
- `RootEventModule` and `TextProducerModule` at ABI 1;
- `RootSummaryModule` and `TextTransformModule` in the Python package;
- zero errors.

If the command reports an ABI tag mismatch, rebuild both Cascade and the plugin
with the same compiler, ROOT installation, standard library, and build mode.

## 7. Run the mixed DAG

```bash
cascade module list
cascade dag run workflow.yaml
```

The DAG is:

```text
root_cpp ──> root_python
text_cpp ──> text_python
```

Successful output:

```text
cli-output/
  events.root
  events_manifest.json
  events_summary.json
  message.json
  message_upper.json
  mixed_pipeline.dot
```

The Python controller runner remains useful when embedding the workflow:

```bash
python3 run_pipeline.py --output example-output
```

`RootSummaryModule` uses PyROOT when it is installed. Otherwise it consumes the
portable manifest committed alongside the ROOT file.

## 8. Run with crash isolation

```bash
python3 run_pipeline.py --isolated --output isolated-output
```

Every module now runs in a subprocess. The output should be equivalent. The parent
controller survives fatal child signals and restores pre-existing files if a child
dies after beginning output promotion.

## 9. Inspect the API

```python
from cascade import py_amcm

controller = py_amcm()
print(controller.get_list_available_modules())

module = controller.register_module("TextProducerModule", "producer")
module.set_output_directory("manual-output")
module.set_cache_directory("manual-output/.cache")
module.set_param("message", "hello")
module.set_param("force_run", True)

result = controller.run_module("producer")
print(result.status, result.phase, result.message)
```

Remove `force_run=True` to exercise normal snapshot-cache skipping.

## Next steps

- [Writing analysis modules](module-authoring.md)
- [Parameters](parameters.md)
- [Configuration schema](configuration.md)
- [AnalysisManager](analysis-manager.md)
- [Execution contract](execution.md)
- [DAG execution](dag.md)
- [Plotting](plotting.md)
- [Plugin development and distribution](plugins.md)
- [Troubleshooting](troubleshooting.md)
