# FAQ

## Is Cascade a ROOT replacement?

No. ROOT supplies I/O, trees, histograms, RDF, and plotting primitives. Cascade
adds module, configuration, execution, caching, output, DAG, and plugin contracts
around them.

## Should a new analysis module be C++ or Python?

Use C++ for event-heavy loops, ROOT-native algorithms, and libraries already
implemented in C++. Use Python for orchestration, summaries, light transformations,
and ecosystem integration. One verified package can contain both.

## Why are Python modules verified too?

Python plugin code executes with the user's privileges just like C++ plugin code.
The manifest provides file integrity and deterministic discovery for both
languages. Publisher signing is optional; use `--require-signed` when
authentication is required for both C++ and Python packages.

## Why is my successful module `Skipped`?

`Skipped` means `dry_run` was enabled or the snapshot already exists. Inspect
`RunResult.message`. Use `force_run` for an intentional rerun.

## Does `force_run` disable cache writes?

No. It bypasses cache lookup for that run, then records the successfully committed
snapshot.

## Why must outputs use a staging helper?

The framework cannot roll back a file it does not know about. Staging registers the
final path and keeps publication coupled to lifecycle/cache success.

## Can modules write outside the output root?

Not through `StageOutput`, `FinalOutput`, `stage_output`, or `final_output`. This
keeps the transaction bounded. Direct application writes are possible but are not
protected.

## Does isolated execution return modified module fields?

No. It returns only the serialized `RunResult`; committed files and cache updates
persist. Use in-process execution if callers require mutated object state.

## Is subprocess isolation a sandbox?

No. It contains crashes and cancellation but does not restrict filesystem, network,
or process privileges.

## Can C++ and Python modules share the same class name?

No. Names must be unique across active plugin roots so unified registration is
unambiguous.

## Do plugins need rebuilding after every Cascade update?

Rebuild whenever the integer ABI or full ABI tag changes. `cascade doctor plugins`
reports both mismatches.

## Why require `schema_version`?

It prevents an old config from silently being interpreted under changed semantics.
Analysis config currently requires version 1; parameter files do not.

## What belongs in a parameter?

Any value that changes output identity: input/config names, thresholds,
systematics, calibration versions, algorithm choices, and output names. Explicit
parameters improve cache correctness and provenance.

## Can two modules run concurrently?

Different instances can, but they must not collide on final output paths or unsafe
external resources. One module instance serializes its own runs.

## Where are logs and caches stored?

Defaults:

```text
Snapshot cache: ~/.cache/cascade/snapshot_cache
Module provenance: OUTPUT/.cascade/provenance/modules (successful runs)
Terminal provenance: CACHE/provenance/modules (skipped/failed/interrupted runs)
Workflow provenance: ~/.cache/cascade/provenance/workflows
```

Override roots with execution-context setters or `CASCADE_CACHE_DIR`. The CLI
workflow `provenance` field and `--provenance` select an exact workflow path.

## What should I run before publishing a plugin?

```bash
cascade doctor plugins
```

Then run at least one normal and one isolated smoke workflow against the exact
installation prefix being deployed.
