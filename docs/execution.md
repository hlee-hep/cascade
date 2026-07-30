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
| `Commit` | Promotes staged output, records snapshot, removes transaction backups |

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
5. atomically record the snapshot hash;
6. remove the staging directory, backups, and journal.

If promotion or cache update fails, promoted files are removed and originals are
restored. During isolated execution, the parent can replay the on-disk journal
after a fatal child signal.

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
- execution state affecting output identity.

Python modules may extend deterministic state with `snapshot_state()`.

Normal behavior:

```text
hash absent  -> execute -> commit output -> record hash -> Done
hash present -> Skipped at Check
force_run    -> execute even if hash is present
dry_run      -> Skipped before execution
```

Use a stable code hash supplied by the plugin build. Make external dataset and
calibration identifiers explicit parameters. Otherwise the cache cannot recognize
that an input changed.

Cache files are locked for concurrent access and replaced atomically. Output file
names are not globally locked: concurrent modules must not target the same final
path.

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

The parent reserves the run and staging directory, then forks. The child executes
the normal lifecycle and returns a bounded status message through a pipe. The
parent converts:

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

Isolation currently requires POSIX `fork()`. It contains crashes but does not
restrict filesystem, network, or process privileges and is not a security sandbox.

## Concurrency

- A module instance serializes its own runs.
- Different module instances may run concurrently.
- Registry/controller metadata is protected for concurrent access.
- DAG structure cannot be mutated while execution is active.
- Modules remain responsible for external resource conflicts and ROOT operations
  that are not thread-safe.

Give concurrently executable modules distinct output paths and avoid shared mutable
globals.

## Run logs

The controller records module name, basename, code hash, resolved parameters,
status, failed phase, and message.

C++:

```cpp
controller.SaveRunLog();
```

Python:

```python
controller.save_run_log_all()
```

Python logs default to `~/.cache/cascade/run_logs`. Override with
`CASCADE_RUN_LOG_DIR` or the `log_dir` argument.

Do not store secrets in module parameters because resolved parameters appear in run
logs.
