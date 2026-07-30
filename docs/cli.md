# Command-line interface

The `cascade` command exposes installation diagnostics, signed module discovery,
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
plugin roots, and trust store. `doctor plugins` verifies signed manifests,
hashes, package boundaries, Python class declarations, and the complete C++ ABI
tag.

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
selects the signed C++ or Python class.

Workflow-relative paths include `output_directory`, `cache_directory`,
`param_file`, and `dot`. Parameter values themselves are not rewritten.
`--fail-fast` and `--keep-going` override the file's failure policy.

The mixed plugin contains a runnable
[`workflow.yaml`](../examples/plugins/mixed_pipeline/workflow.yaml).

## JSON output

`info`, `doctor env`, `module list`, `module run`, and `dag run` support
`--json`. Framework and module stdout is redirected to stderr during execution
so stdout remains a single JSON document.

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
