# Command-line interface

The `cascade` command exposes installation diagnostics, verified module discovery,
single-module execution, mixed-language DAG workflows, and a compatibility
adapter for ROOT macros.

## Inspect an installation

```bash
cascade --version
cascade info
cascade info --json
cascade doctor env
cascade doctor plugins
```

`doctor env` checks the imported Cascade runtime, ROOT executable, OpenSSL,
plugin roots, and trust store. `doctor plugins` reports `VERIFIED` or `SIGNED`
packages and checks manifests, hashes, package boundaries, Python class
declarations, and the complete C++ ABI tag.

Require trusted publisher signatures for controller-backed commands by placing
the global option before the command:

```bash
cascade --require-signed doctor plugins
cascade --require-signed module list
cascade --require-signed dag run workflow.yaml
```

## List and run modules

```bash
cascade module list
cascade module list --language python --tag root

cascade module run TextProducerModule \
  --name producer \
  --output-directory output \
  --cache-directory output/.cache \
  --set force_run=true \
  --set repeat=5
```

Use `--params parameters.yaml` for a YAML or JSON parameter mapping.
Command-line `--set` values are applied afterward and therefore override the
file. Add `--isolated` to execute the module in a subprocess.

Successful and cache/dry-run skipped results exit with status 0. Failed or
interrupted results exit with status 1.

## DAG workflow files

Run a workflow with:

```bash
cascade dag run workflow.yaml
cascade dag run workflow.yaml --keep-going --dot output/final.dot
cascade dag run workflow.yaml --json
```

The schema is:

```yaml
schema_version: 1

output_directory: output
cache_directory: output/.cache
fail_fast: true
dot: output/workflow.dot
provenance: output/workflow-provenance.json

modules:
  - module: ProducerModule
    name: producer
    isolated: false
    params:
      force_run: true
      output_tag: nominal

  - module: ConsumerModule
    name: consumer
    dependencies: [producer]
    param_file: consumer-params.yaml

links:
  - from: producer.output_tag
    to: consumer.input_tag
```

All fields are validated and unknown fields are rejected. Module names must be
unique. Dependencies and parameter links use instance names, while `module`
selects the verified C++ or Python class.

Workflow-relative paths include `output_directory`, `cache_directory`,
`param_file`, `dot`, and `provenance`. Parameter values themselves are not rewritten.
`--fail-fast` and `--keep-going` override the file's failure policy.
`--provenance PATH` overrides the workflow field.

The mixed plugin contains a runnable
[`workflow.yaml`](../examples/plugins/mixed_pipeline/workflow.yaml).

## JSON output

`info`, `doctor env`, `doctor plugins`, `module list`, `module run`, and `dag run`
support `--json`. Framework and module stdout is redirected to stderr during
execution so stdout remains a single JSON document.

Single-module JSON includes the module `run_id` and `provenance` path. DAG JSON
includes the workflow `provenance` path.

## Inspect and replay past runs

The provenance time-machine commands list, inspect, compare, and replay recorded
runs:

```bash
cascade history
cascade inspect module-1234
cascade diff module-1234 module-5678
cascade replay module-1234
```

Run arguments accept either an exact run ID or a provenance manifest path.
Without `--root`, run IDs are discovered below the current directory and the
default Cascade cache. Add one or more explicit search roots when outputs live
elsewhere:

```bash
cascade history --root results --kind module --limit 10
cascade inspect workflow-1234 --root results --json
cascade diff module-1234 module-5678 --root results --json
```

`diff` omits per-run timestamps, run IDs, and manifest linkage paths. It reports
changes in reproducibility-relevant state such as module identity, parameters,
runtime, results, and artifacts.

`replay` currently supports module-run manifests. It restores the recorded
module class, instance name, parameters, output/cache roots, and isolation mode.
All of these except the module class can be overridden:

```bash
cascade replay module-1234 \
  --root results \
  --set threshold=25 \
  --output-directory replay-output \
  --no-isolated
```

Secret parameter values are redacted in provenance and cannot be recovered.
Replay stops and names every redacted value that must be supplied again with
`--set`.

## ROOT macro compatibility

The structured command is:

```bash
cascade macro run examples/RootMacroExample.C \
  --set n=1000 \
  --set mode='"fast"'
```

The older form remains supported:

```bash
cascade --macro examples/RootMacroExample.C --set n=1000
```

Parameters are written to a temporary JSON document and passed as the macro's
first string argument. `--yaml` adds its absolute path as `_yaml_path`;
`--extra` appends additional string arguments. Quotes and backslashes are
escaped before constructing the ROOT expression.

## Exit status

| Status | Meaning |
| --- | --- |
| `0` | Command and requested analysis completed successfully |
| `1` | Diagnostic, validation, module, or DAG failure |
| `2` | Invalid command-line syntax |
| `130` | Interrupted with `SIGINT` |
