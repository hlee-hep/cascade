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
cascade doctor runtime
cascade doctor plugins
```

`doctor env` checks the imported Cascade runtime, ROOT executable, OpenSSL,
plugin roots, and trust store. `doctor plugins` reports `VERIFIED` or `SIGNED`
packages and checks manifests, hashes, package boundaries, Python class
declarations, and the complete C++ ABI tag.

`doctor runtime` resolves output/cache roots, input and output hash policies, DAG
worker count, progress interval, isolation timeout, resource limits, both worker
executables, and the isolated Python runtime. It also checks ownership, executable
permissions, and writable parent directories. Use `--json` for deployment gates.

## Install and locate plugins

The recommended installation command combines build, staged verification,
publish, and persistent prefix registration:

```bash
cascade plugin install ./my-plugin
cascade plugin install ./my-plugin --prefix /data/cascade-plugins
```

The default plugin prefix is `~/.local`. The source directory must contain a
Cascade-compatible `SConstruct`. Installation first targets a temporary
directory inside the destination prefix. Cascade publishes the package only
after its manifest, hashes, Python declarations, C++ ABI, and active signature
policy pass verification. Existing package directories are restored if publish
or configuration update fails.

Signed installation uses explicit key arguments:

```bash
cascade --require-signed plugin install ./my-plugin \
  --private-key /secure/publisher-private.pem \
  --public-key /provisioning/publisher-public.pem
```

Persistent prefixes are managed separately when packages were installed by an
external tool:

```bash
cascade plugin path add /opt/experiment-plugins
cascade plugin path list
cascade plugin path remove /opt/experiment-plugins
```

`path add --create` creates a missing prefix. Configuration is stored in
`${XDG_CONFIG_HOME:-~/.config}/cascade/config.json`. `CASCADE_CONFIG_FILE`
selects another file for tests and isolated environments.

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

Runtime policy can be scoped to this invocation without exporting environment
variables:

```bash
cascade module run TextProducerModule \
  --input-hash metadata \
  --output-hash full \
  --timeout 900 \
  --progress-interval-ms 250 \
  --explain-cache
```

`--explain-cache` prints `hit`, `miss`, `bypassed`, or `not_checked` plus the exact
reason. JSON output always contains `cache_decision` and `cache_reason`.

Use `--params parameters.yaml` for a YAML or JSON parameter mapping.
Command-line `--set` values are applied afterward and therefore override the
file. Add `--isolated` to execute the module in a subprocess.

Successful and cache/dry-run skipped results exit with status 0. Failed or
interrupted results exit with status 1.

## Inspect and prune snapshot caches

Snapshot cache commands use `CASCADE_CACHE_DIR` or the default
`~/.cache/cascade/snapshot_cache`. Select another root explicitly when a module
or workflow used a custom cache directory:

```bash
cascade cache list
cascade cache list --cache-directory output/.cache --module selection
cascade cache explain selection SNAPSHOT_HASH --cache-directory output/.cache
```

`cache explain` reports a hit when the exact module instance and snapshot hash
exist. It also reports the linked provenance path and whether that manifest is
still present.

Pruning removes entries whose recorded provenance manifest no longer exists:

```bash
cascade cache prune --cache-directory output/.cache --dry-run
cascade cache prune --cache-directory output/.cache
```

Legacy hash-only entries have no provenance link and are preserved by the
default prune mode. Use `--all` to remove every matching entry, and optionally
`--module NAME` to restrict the operation. Cache files are read and rewritten
under the same locks used by module execution.

## DAG workflow files

Run a workflow with:

```bash
cascade dag validate workflow.yaml
cascade dag run workflow.yaml
cascade dag run workflow.yaml --keep-going --dot output/final.dot
cascade dag run workflow.yaml --workers 4 --progress
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

`dag validate` loads verified plugins, constructs every module, applies parameter
files and inline values, checks registered parameter types, and wires the DAG
without executing a lifecycle phase. It rejects missing or duplicate
dependencies, self-dependencies, cycles, missing link nodes, links whose source
is not an ancestor of the target, unregistered linked parameters, and conflicting
DOT/provenance paths.

Workflow-relative paths include `output_directory`, `cache_directory`,
`param_file`, `dot`, and `provenance`. Parameter values themselves are not rewritten.
`--fail-fast` and `--keep-going` override the file's failure policy.
`--provenance PATH` overrides the workflow field.

Interactive non-JSON runs show live node transitions automatically when stderr is
a terminal. `--progress` forces event-style progress in redirected logs and
`--no-progress` disables it. Pending nodes say whether they are waiting for
dependencies or an execution lane; running analysis modules include their averaged
manager progress when available. The display is written only to stderr, preserving
machine-readable stdout.

DAG runs accept the same hash, timeout, and progress-interval options as module
runs, plus `--workers N` for the bounded execution pool. These options affect only
the current process invocation and take precedence over the corresponding runtime
environment variables while the command executes.

The mixed plugin contains a runnable
[`workflow.yaml`](../examples/plugins/mixed_pipeline/workflow.yaml).

## JSON output

`info`, all `doctor` commands, plugin management commands, cache
commands, `module list`, `module run`, and DAG commands support `--json`. Framework and module stdout
is redirected to stderr during execution so stdout remains a single JSON
document.

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
