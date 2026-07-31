# Provenance manifests

Cascade records reproducibility data as versioned JSON manifests. This is the
canonical record for module runs and DAG workflows; `save_run_log` remains as a
compatibility alias that writes a workflow manifest.

## Module-run manifest

Every terminal module run produces a `cascade.module-run` document containing:

- run ID, module instance, module metadata, and implementation language;
- Cascade version, plugin ABI version/tag, and ROOT version when available;
- code hash, snapshot hash, and resolved parameters;
- start/finish timestamps and configured output/cache roots;
- status, failed phase, message, isolation, dry-run, and cache-hit state;
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

## Declaring inputs

The framework cannot reliably infer every file opened by user code. Declare
material inputs explicitly:

```cpp
void SelectionModule::Init()
{
    TrackInput(FinalOutput(m_Param.Get<std::string>("input")));
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
that supplied the cached snapshot.

Legacy hash-only C++ YAML sequences and Python JSON lists are read and upgraded
when the cache is next written.
