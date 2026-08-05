# Provenance manifests

Cascade records reproducibility data as versioned JSON manifests. This is the
canonical record for module runs and DAG workflows; `save_run_log` remains as a
compatibility alias that writes a workflow manifest.

## Module-run manifest

Every terminal module run produces a `cascade.module-run` document containing:

- run ID, module instance, module metadata, and implementation language;
- Cascade version, plugin ABI version/tag, and ROOT version when available;
- plugin package, verification status, manifest/artifact hashes, and signer
  fingerprint when available;
- code hash, snapshot hash, and resolved parameters;
- start/finish timestamps and configured output/cache roots;
- status, failed phase, message, isolation, dry-run, cache decision, and exact
  cache reason;
- tracked input artifacts and transactional output artifacts;
- the prior manifest referenced by a cache hit.

Successful manifests are committed with module outputs:

```text
OUTPUT/.cascade/provenance/modules/RUN_ID.json
```

The manifest is staged inside the same `OutputTransaction`, so it cannot describe
an output set that failed to commit. Failed, interrupted, and skipped runs have no
new committed output set and are written under:

```text
CACHE/provenance/modules/RUN_ID.json
```

Output files and directories are discovered automatically from
`StageOutput`/`stage_output`. Regular files receive a SHA-256 digest. Directory
digests are deterministic over sorted relative entries and their content hashes.
Symlinks are hashed by link target and are not followed.

Each artifact record contains `path`, `kind`, `exists`, aggregate `size`,
`hash_mode`, optional `sha256`, and an `identity` object. The identity holds device, inode, nanosecond
mtime, and nanosecond ctime. It is an optimization as well as an audit record: an
exact identity match lets cache validation avoid rereading a large output.

## Declaring inputs

The framework cannot reliably infer every file opened by user code. Declare
material inputs explicitly:

```cpp
void SelectionModule::Init()
{
    TrackInput(FinalOutput(Parameters().Get<std::string>("input")));
}
```

```python
def init(self):
    self.track_input(self.final_output(self.get_param("input")))
```

Local inputs are hashed when the manifest is finalized. URI-like inputs are
recorded without pretending that their remote content was inspected. Dataset
version, calibration tag, query, or other semantic identity should still be a
registered parameter so it participates in the snapshot hash.

## Parameters and secrets

Manifest parameters are normalized to resolved values. Keys containing common
secret terms such as `password`, `token`, `credential`, `private_key`, or
`api_key` are redacted. This is a safety net, not a secret-management system:
prefer environment variables or an external credential provider for secrets.

## Workflow manifest

`cascade.workflow-run` links module manifests to the final DAG state:

- node status, message, and dependency list;
- parameter/data-link relationships;
- module run ID and manifest for each executed node;
- fail-fast policy and aggregate success;
- runtime identity and workflow timing.

Python and mixed workflows write one automatically from `run_dag`. C++ callers
can save the current controller history explicitly:

```cpp
const DAGRunResult result = controller.RunDAG();
const std::string manifest = controller.SaveProvenance();
```

```python
result = controller.run_dag()
print(controller.last_workflow_provenance_path)
```

The CLI accepts an exact workflow path:

```yaml
provenance: output/workflow-provenance.json
```

or:

```bash
cascade dag run workflow.yaml --provenance output/workflow-provenance.json
```

`cascade module run --json` includes `run_id` and `provenance`; `cascade dag run
--json` includes the workflow `provenance` path.

## Time-machine commands

Provenance manifests are also the storage layer for the CLI run-history tools:

```bash
cascade history --root output
cascade inspect RUN_ID --root output
cascade diff EARLIER_RUN LATER_RUN --root output
cascade replay MODULE_RUN --root output
```

History and inspection support module and workflow manifests. Diff ignores
volatile run identity and timing fields so its output focuses on meaningful
configuration, runtime, result, DAG, and artifact changes. Replay restores one
module run through the normal verified-plugin controller; it does not bypass
plugin verification or the standard execution lifecycle.

See [Command-line interface](cli.md#inspect-and-replay-past-runs) for discovery
roots, overrides, JSON output, and redacted-parameter behavior.

## Cache linkage

Snapshot cache entries use schema version 1 records:

```yaml
schema_version: 1
snapshots:
  - hash: 012345...
    provenance: /path/to/the/completed-module-manifest.json
```

The cache remains a fast execution decision index. The manifest is the descriptive
record. On a cache hit, the new skipped-run manifest points to the completed run
that supplied the cached snapshot. Before accepting the hit, Cascade validates the
completed manifest, snapshot hash, output root, and every recorded output. Stable
file identities avoid rehashing unchanged outputs; changed identities fall back to
validation under the policy recorded in `hash_mode`. This prevents a `metadata` or
`none` artifact from being reread in full merely because its inode or timestamps
changed. The final path component is kept unresolved so a recorded symlink remains
a symlink during validation.

Tracked input identity defaults to filesystem metadata so large ROOT files are not
read solely to make a cache decision. `CASCADE_INPUT_HASH_MODE=full` records and
uses SHA-256 instead, while `auto` hashes regular inputs up to 64 MiB. Metadata
mode records identity without proving byte equality. Directory capture still walks
the directory tree, even when regular-file contents are not hashed.

Output records use the independent `CASCADE_PROVENANCE_HASH_MODE`. Its default,
`full`, allows a moved or replaced output to be accepted only if the recomputed
content hash matches. With `metadata` or `none`, kind and size are the remaining
checks after an identity change. See
[Runtime reliability and performance](runtime-reference.md#output-provenance-policy)
for the complete decision table.

Legacy hash-only C++ YAML sequences and Python JSON lists are read and upgraded
when the cache is next written.
