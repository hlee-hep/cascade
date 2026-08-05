# Execution contract

## Lifecycle

Every module run follows the same framework-owned sequence:

```text
Begin context
    │
    ▼
Init ──> Check ──> Execute ──> Finalize ──> Commit
    │       │          │           │           │
    └───────┴──────────┴───────────┴───────────┴──> RunResult
```

The user implements `Init`, `Execute`, and `Finalize`. `Check` and `Commit` are
framework phases.

| Phase | Framework behavior |
| --- | --- |
| `Init` | Starts `ExecutionContext`, recreates analysis-manager state, invokes module initialization |
| `Check` | Handles `dry_run`, computes snapshot hash, checks cache, applies `force_run` |
| `Execute` | Runs analysis logic |
| `Finalize` | Runs user serialization/finalization logic |
| `Commit` | Stages provenance, promotes output, records snapshot linkage, removes transaction backups |

An exception is caught at its phase boundary. The framework calls the optional
failure hook, rolls back active output state, and returns `Failed`.

Cancellation is checked between phases and must also be checked cooperatively by
long-running user loops.

## RunResult

C++:

```cpp
RunResult result = controller.RunAModule("selection");

if (result.Failed())
{
    std::cerr << ToString(result.Phase) << ": " << result.Message << '\n';
}
```

Python:

```python
result = controller.run_module("selection")

if result.failed():
    raise RuntimeError(f"{result.phase}: {result.message}")
```

Statuses:

| Status | Meaning | Output action |
| --- | --- | --- |
| `Done` | Lifecycle and commit completed | Staged output is visible |
| `Skipped` | Dry run or matching snapshot cache | Staged output is discarded |
| `Interrupted` | Cancellation or global interrupt | Staged output is rolled back |
| `Failed` | Exception, commit error, invalid isolated result, or child crash | Staged/promoted output is rolled back |

`Pending`, `Initializing`, `Running`, and `Finalizing` are observable in-progress
states rather than terminal results.

Every result also carries `CacheDecision`/`cache_decision` and
`CacheReason`/`cache_reason`. Decisions are `not_checked` for a dry run or a run
that never reached `Check`, `bypassed` for `force_run`, `miss` when execution was
required, and `hit` when a completed snapshot and every recorded output matched.
The reason preserves the exact stale-manifest or output-validation failure instead
of reducing every rerun to a generic cache miss.

## ExecutionContext

Every run owns:

- a unique run ID;
- an output root;
- a cache root;
- a cancellation token;
- logger access;
- an output transaction.

Configure module instances before execution:

```cpp
module->SetOutputDirectory("results");
module->SetCacheDirectory("results/.cache");
```

```python
module.set_output_directory("results")
module.set_cache_directory("results/.cache")
```

Construction-time defaults:

| Variable | Default |
| --- | --- |
| `CASCADE_OUTPUT_DIR` | Current working directory |
| `CASCADE_CACHE_DIR` | `~/.cache/cascade/snapshot_cache` |

Roots cannot be changed during an active run. The output root contributes to the
snapshot identity; the cache root only chooses where snapshots are stored.

## Transactional outputs

Register every protected output by asking the context for a staging path:

```cpp
auto staged = StageOutput("histograms.root");
Am()->WriteHistograms(staged.string());
```

```python
with self.stage_output("summary.json").open("w", encoding="utf-8") as output:
    json.dump(summary, output)
```

Multiple staged files form one transaction. Commit order:

1. confirm every staged path was created;
2. write the promotion journal;
3. move existing final files to transaction backups;
4. promote staged files;
5. commit the module provenance manifest with the output set;
6. atomically record the snapshot hash and manifest linkage;
7. remove the staging directory, backups, and journal.

This is rollback-capable failure atomicity, not simultaneous multi-path visibility
or a power-loss durability guarantee. A reader outside Cascade's output locks may
observe promotion in progress, and abrupt parent-process or machine failure is not
followed by a general startup recovery scan.

If promotion or cache update fails, promoted files are removed and originals are
restored. During isolated execution, the parent can replay the on-disk journal
after a fatal child signal. Recovery checks the promoted file identity before
rolling it back, so it does not overwrite output committed later by another run.

Both relative and absolute requests must resolve inside the configured output root.
The output root itself is not a valid file target.

`FinalOutput`/`final_output` resolves an already committed file inside the same
root, typically for an upstream DAG product.

### Outside the transaction

The framework cannot roll back:

- files written without a staging helper;
- database mutations;
- network requests;
- emails or messages;
- state in external services.

Delay such operations until durable outputs exist, make them idempotent, or build
an application-specific transaction.

## Snapshot cache

The snapshot is derived from:

- module basename;
- registered parameter values;
- registered `AnalysisManager` state;
- code-version hash;
- execution state affecting output identity;
- tracked input identity according to `CASCADE_INPUT_HASH_MODE`.

Python modules may extend deterministic state with `snapshot_state()`.

Normal behavior:

```text
hash absent  -> execute -> commit output -> record hash -> Done
hash present -> Skipped at Check
force_run    -> execute even if hash is present
dry_run      -> Skipped before execution
```

The verified loader uses the plugin artifact SHA-256 as the stable code hash.
Make external dataset and calibration identifiers explicit parameters. Otherwise
the cache cannot recognize that an input changed.

Cache files are locked for concurrent access and replaced atomically. Final output
paths use hierarchical inter-process locks from promotion through cache recording
and transaction completion. A directory output conflicts with every output below
it, while sibling files may still commit concurrently. Concurrent modules should
still use distinct final paths because the last successful publisher wins.

`TrackInput`/`track_input` artifacts are part of the snapshot hash. Input hashing
defaults to `CASCADE_INPUT_HASH_MODE=metadata`, recording device, inode, size,
nanosecond modification time, and change time without reading the complete file.
Use `full` for SHA-256 content identity or `auto` to hash regular files up to
64 MiB and use metadata for larger inputs. This policy is shared by C++ and Python
modules and applies to tracked-input provenance as well.

A cache entry is accepted only when its completed provenance manifest matches the
snapshot and output root and its recorded outputs still match their committed
identities or content hashes. Missing, replaced, or corrupted output invalidates
the entry and causes a normal rerun. Cache index and module-provenance reads are
capped at 16 MiB to bound malformed local-state parsing.

Output artifact records retain the hash policy used when they were committed.
After a filesystem identity change, validation recaptures with that same policy:
`full` may rehash content, while `metadata` and `none` never pay an accidental
full-content read. Symlink outputs are validated as symlinks without resolving the
final component to their target.

Metadata input identity is the performance-oriented default, not a cryptographic
content guarantee. Use `full` on filesystems with weak timestamp semantics, when
another tool may deliberately preserve metadata while rewriting a file, or for an
archival/release run. Directory inputs are traversed even in metadata mode, so a
versioned dataset manifest is usually a better tracked input than a directory with
millions of entries.

## Cancellation

Request cancellation:

```cpp
module->RequestCancellation();
```

```python
module.request_cancellation()
```

Check it in long loops:

```cpp
if (IsCancellationRequested()) return;
```

```python
if self.is_cancellation_requested():
    return
```

Global SIGINT is reflected by the token. After user code returns, the framework
observes cancellation before commit and produces `Interrupted`.

Subprocess cancellation sends `SIGTERM`; a child that does not exit is escalated
to `SIGKILL`.

## Subprocess isolation

C++:

```cpp
RunResult result = controller.RunAModuleIsolated("module_instance");
```

Python:

```python
result = controller.run_module_isolated("module_instance")
```

The parent reserves the run and staging directory, then starts a clean worker with
`exec()`. The worker reopens the selected package manifest, verifies only the
requested module artifact and its signature policy, confirms that the hashes still
match, reconstructs the module, and executes the normal lifecycle. It does not scan
or load unrelated installed packages. A bounded status message returns through a
pipe. The parent converts:

- fatal signals;
- abnormal exit;
- truncated or invalid replies;
- cancellation;

into a terminal `RunResult`.

Committed files and cache records cross the boundary. These do not:

- module member variables;
- Python object mutations;
- `AnalysisManager` instances;
- open handles intended for later parent use;
- child-only global state.

Design isolated pipelines around files or another explicit durable protocol.

Only modules discovered from verified plugin packages can run in isolation; an
arbitrary in-memory module handle cannot be reconstructed safely after `exec()`.
Set `CASCADE_ISOLATED_TIMEOUT_SECONDS` to a positive number to enforce a worker
deadline; zero or an unset value means no deadline. Worker executables must resolve
to absolute, executable, owner/root-controlled files that are not group/world
writable. Their parent directories, and the isolated Python runtime's parent chain,
must not be group/world writable unless the directory is sticky and owned by root
or the current user, such as `/tmp`. Workers close inherited descriptors, enable Linux `no_new_privs`, use a
private umask, and do not inherit loader-injection variables such as `LD_PRELOAD`
or `LD_AUDIT`. The Python worker starts in interpreter-isolated mode, ignores the
inherited `PYTHONPATH`, and imports Cascade only from the canonical runtime
directory. Custom `PYTHONDIR` layouts can provide that parent directory through
`CASCADE_PYTHON_RUNTIME_DIR`; it must be absolute and owner/root controlled.
`cascade doctor runtime` reports the exact resolved paths and applies the same
ownership and directory-safety checks without starting a worker.

Optional positive worker limits are `CASCADE_WORKER_MEMORY_LIMIT_MB`,
`CASCADE_WORKER_FILE_SIZE_LIMIT_MB`, `CASCADE_WORKER_MAX_PROCESSES`, and
`CASCADE_WORKER_MAX_OPEN_FILES`. These are defense-in-depth controls. Isolation
contains crashes but still permits ordinary filesystem and network access and is
not a complete security sandbox.

Output provenance hashing defaults to `CASCADE_PROVENANCE_HASH_MODE=full`. Use
`metadata` to avoid reading complete output artifacts, or `none` to record only
existence, kind, and size where throughput matters more than content fingerprints.
Tracked inputs use the separate `CASCADE_INPUT_HASH_MODE` policy above. Cache
histories keep 256 snapshots per module by default; override that with
`CASCADE_CACHE_MAX_SNAPSHOTS` (`0` means unlimited).

Full hashes are streamed in 1 MiB chunks and reused within the process when the
file device, inode, size, modification time, and change time are unchanged. This
avoids repeated reads when independent modules track the same immutable input. The
cache holds 1024 identities by default; set
`CASCADE_PROVENANCE_HASH_CACHE_ENTRIES=0` to disable it or choose another bound.
The digest cache is not persisted across processes. With output hashing set to
`metadata` or `none`, a later identity change cannot be resolved by byte comparison;
cache validation then has only the recorded kind and size. This is an explicit
throughput-versus-integrity tradeoff.

## Concurrency

- A module instance serializes its own runs.
- Module parameters are frozen from run reservation through completion; concurrent
  reads use an immutable snapshot and writes fail instead of changing a live run.
- Different module instances may run concurrently.
- Registry/controller metadata is protected for concurrent access.
- DAG structure cannot be mutated while execution is active.
- In-process `AnalysisManager` modules share one process-wide ROOT execution lane.
- Isolated nodes and C++ modules without analysis managers use bounded DAG worker
  lanes. In-process Python nodes share the ROOT-safe serial lane.
- `CASCADE_DAG_MAX_WORKERS` bounds a DAG's concurrent work and defaults to detected
  hardware concurrency.

Give concurrently executable modules distinct output paths and avoid shared mutable
globals.

Progress state is updated for every callback, while terminal rendering is throttled
to once every 200 ms. Set `CASCADE_PROGRESS_INTERVAL_MS=0` to render every update or
choose a different non-negative interval.

## Provenance and run-log compatibility

The canonical execution record is a versioned JSON provenance manifest. It
combines module metadata, code/snapshot identity, resolved parameters, lifecycle
result, artifact hashes, cache lineage, and DAG relationships.

C++:

```cpp
controller.SaveRunLog();
```

Python:

```python
controller.save_run_log_all()
```

These names remain compatibility aliases and now write `cascade.workflow-run`
JSON. Prefer `SaveProvenance()` / `save_provenance()`. See
[Provenance manifests](provenance.md) for locations, input tracking, redaction,
and cache linkage. The complete setting and decision reference is
[Runtime reliability and performance](runtime-reference.md).
