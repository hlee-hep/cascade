# Mixed C++/Python plugin example

This package is the reference implementation for a distributable Cascade plugin.
It contains two independent DAG branches:

```text
TextProducerModule (C++) ──> TextTransformModule (Python)
RootEventModule (C++) ─────> RootSummaryModule (Python)
```

## What each module demonstrates

| Module | Demonstrates |
| --- | --- |
| `TextProducerModule` | Typed C++ parameters and transactional JSON output |
| `TextTransformModule` | Reading a committed upstream file and staging Python output |
| `RootEventModule` | ROOT `TFile`/`TTree` output plus a multi-file transaction |
| `RootSummaryModule` | Optional PyROOT with a portable JSON fallback |

The pipeline runner demonstrates unified registration, per-module output/cache
roots, controller-managed DAG nodes, result checking, DOT export, and isolated
execution.

## Prerequisite

Build and install Cascade, then set:

```bash
export CASCADE_PREFIX=/your/cascade/prefix
export PATH="${CASCADE_PREFIX}/bin:${PATH}"
export PYTHONPATH="${CASCADE_PREFIX}/lib:${PYTHONPATH}"
export LD_LIBRARY_PATH="${CASCADE_PREFIX}/lib:${LD_LIBRARY_PATH}"
```

## Development key

```bash
openssl genpkey -algorithm Ed25519 -out plugin_private.pem
openssl pkey -in plugin_private.pem -pubout -out plugin_public.pem
```

The local `.gitignore` excludes these development keys. Use an external protected
key for real distribution.

## Build and install

```bash
CASCADE_PLUGIN_PACKAGE=mixed_pipeline \
CASCADE_PLUGIN_PRIVATE_KEY="$PWD/plugin_private.pem" \
CASCADE_PLUGIN_PUBLIC_KEY="$PWD/plugin_public.pem" \
scons install
```

The source-tree `SConstruct` delegates to Cascade's plugin template. It replaces
`@BASENAME@` and `@VERSION_HASH@`, compiles C++ modules, installs Python sources,
generates separate manifests, and signs both.

Verify:

```bash
cascade doctor plugins
cascade module list
```

## Run

In-process:

```bash
python3 run_pipeline.py --output example-output
```

Isolated:

```bash
python3 run_pipeline.py --isolated --output isolated-output
```

The same four-module pipeline can be run declaratively:

```bash
cascade dag run workflow.yaml
```

Expected files:

```text
events.root
events_manifest.json
events_summary.json
message.json
message_upper.json
mixed_pipeline.dot
```

Every data product is registered with `StageOutput`/`stage_output`. The final paths
become visible only after lifecycle and cache commit.

## PyROOT behavior

`RootSummaryModule` imports PyROOT lazily:

- when available, it reads the `events` tree directly;
- otherwise, it reads `events_manifest.json`, which `RootEventModule` commits in
  the same transaction as `events.root`.

The selected backend appears in `events_summary.json`.

## Try modifications

- Change `message` or `repeat` on `TextProducerModule`.
- Change `events` or `scale` on `RootEventModule`.
- Remove `force_run=True` in `run_pipeline.py` and run twice to observe `Skipped`.
- Raise an exception after staging a file and verify no partial final file appears.
- Add another Python summary node depending on both DAG branches.
- Run one native-risk module with isolation while keeping light Python modules
  in-process.

See:

- [Module authoring](../../../docs/module-authoring.md)
- [Execution contract](../../../docs/execution.md)
- [Plugin guide](../../../docs/plugins.md)
