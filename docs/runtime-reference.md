# Runtime reliability and performance reference

This page collects the runtime controls that affect correctness, cache behavior,
throughput, and isolation. The shorter topic guides remain the best place to learn
the APIs; use this page when choosing production settings or diagnosing a run.

## Guarantees and boundaries

Cascade guarantees the following for outputs registered with
`StageOutput`/`stage_output`:

- all registered outputs from one module run participate in one rollback-capable
  transaction;
- an interrupted or failed promotion restores the previously committed output set;
- concurrent transactions lock overlapping output paths, including directory/child
  conflicts;
- a successful snapshot-cache entry links to a completed module provenance manifest;
- a cache hit is accepted only after the manifest and its recorded outputs are
  validated;
- C++ and Python modules use the same C++ lifecycle, snapshot-cache, transaction,
  provenance, plugin-verification, and DAG services.

The guarantees do not extend to direct writes, databases, network calls, messages,
or other external side effects. Plugin verification authenticates package identity
and, when signatures are required, its publisher. It does not prove that plugin
code is safe. Subprocess isolation contains crashes and stale process state, but it
is not a filesystem or network sandbox.

The output transaction is failure-atomic for handled commit errors and isolated
worker failures; multiple filesystem paths cannot become visible in one indivisible
rename. Readers that do not participate in Cascade's output locks may briefly
observe promotion in progress. The journal is a rollback aid, not a power-loss
durability protocol, and an abrupt parent-process or machine crash is not followed
by a general startup recovery scan.

## Cache decision sequence

Parameters are frozen before `Init`. After `Init` returns, Cascade checks
cancellation and enters `Check`, which follows this order:

1. return `Skipped` for `dry_run`, without computing a snapshot;
2. capture manager state and tracked-input identity using
   `CASCADE_INPUT_HASH_MODE`;
3. hash the complete snapshot;
4. bypass lookup when `force_run=true`;
5. find the snapshot in the module cache index;
6. load the linked completed provenance manifest;
7. require status `Done`, the same snapshot hash, and the same output root;
8. validate every recorded output;
9. return a cache-hit `Skipped`, or execute normally when any check fails.

Output validation first compares the recorded filesystem identity: device, inode,
size, nanosecond modification time, and change time. An exact match avoids reading
the output. If the identity changed, Cascade captures the artifact again and
compares it under the `hash_mode` stored with that artifact. `full` recomputes a
digest; `metadata` and `none` do not read regular-file contents. Legacy manifests
without `hash_mode` use `full` only when they contain a digest.

`RunResult`, Python results, CLI JSON, and module provenance retain the decision and
reason. Use `cascade module run ... --explain-cache` for the same information in
human-readable output.

A malformed, missing, moved, or incompatible cache entry is treated as a miss. It
does not authorize a partial result. `force_run` bypasses lookup but still records
the new completed snapshot and provenance.

## Input identity policy

`CASCADE_INPUT_HASH_MODE` applies to explicit
`TrackInput`/`track_input` artifacts in both the snapshot and provenance.

| Mode | Regular file behavior | Best fit | Main tradeoff |
| --- | --- | --- | --- |
| `metadata` | Device, inode, size, mtime, and ctime | Large local ROOT files and normal iterative analysis | Does not prove byte-for-byte identity |
| `auto` | SHA-256 at or below 64 MiB; metadata above it | Mixed small configuration and large data inputs | Large inputs retain metadata semantics |
| `full` | Streamed SHA-256 | Reproducible releases, archival validation, weak or unfamiliar filesystems | Reads every byte at least once per process identity |

The default is `metadata`, so the first run does not read a multi-gigabyte ROOT file
solely to decide whether the module is cached. Metadata identity is strong for
ordinary local filesystems, but it is a policy choice rather than a cryptographic
content guarantee. Use `full` when files may be rewritten while preserving metadata,
when filesystem timestamp semantics are weak, or when byte-level reproducibility is
required.

Tracked input directories are always traversed to build a deterministic entry
fingerprint. `metadata` avoids reading regular-file contents,
but a directory with millions of entries can still be expensive to enumerate. For
large datasets, track a versioned manifest file or dataset identifier instead of a
whole directory when that represents the real semantic input.

URI-like inputs are recorded but not fetched. Put the remote dataset version,
object generation, query, or checksum in a registered parameter.

## Output provenance policy

`CASCADE_PROVENANCE_HASH_MODE` controls committed output artifact records.

| Mode | Recorded validation data | Cache behavior after identity changes |
| --- | --- | --- |
| `full` | Kind, size, filesystem identity, SHA-256 | Rehash and compare content; unchanged bytes remain valid |
| `metadata` | Kind, size, filesystem identity; deterministic directory metadata fingerprint | Kind and size can be checked, but byte equality is unavailable |
| `none` | Existence, kind, size, and top-level identity | Only kind and size remain after an identity change |

The default is `full`. Keep it for release artifacts and outputs whose cached reuse
must survive moves or replacements safely. Choose `metadata` only when output hashing
is a measured bottleneck and metadata-level validation meets the workflow's risk
model. `none` is intended for disposable or independently validated outputs.

Full regular-file hashes are streamed in 1 MiB chunks. A bounded, process-local
cache reuses a digest while device, inode, size, mtime, and ctime are unchanged.
The cache is not persisted between processes, and it does not remove the first full
read in a new process.

## Transaction and recovery sequence

Each run writes below a private staging directory. Commit then:

1. verifies that every staged target exists;
2. acquires hierarchical locks for final output paths;
3. writes a promotion journal;
4. moves existing final artifacts to private backups;
5. promotes the staged artifacts and staged provenance manifest;
6. refreshes committed filesystem identities in the provenance manifest;
7. atomically records snapshot-to-provenance linkage;
8. removes the journal, backups, and staging tree.

If a step fails, the transaction removes only artifacts whose recorded promoted
identity still matches and restores backups. This identity check prevents recovery
from overwriting a newer commit made by another run. A directory target conflicts
with every descendant path; unrelated sibling files may commit concurrently.

Avoid two successful modules publishing the same final path. Locking prevents a
torn commit, but it cannot decide which publisher is semantically correct; the last
successful publisher wins.

When a cached output identity changes, the validator preserves the policy that
created the record. A `full` directory can still require a complete recursive walk;
`metadata` walks directory entries without reading regular-file contents, and
`none` checks only the top-level artifact fields available under that policy.

## DAG scheduling lanes

| Lane | Typical node | Concurrency rule |
| --- | --- | --- |
| `Serial` | Generic callback or in-process Python module | Runs exclusively after active pooled work drains |
| `Root` | In-process module using `AnalysisManager` | One process-wide ROOT node at a time; may overlap ROOT-free pooled work |
| `Parallel` | C++ module with `UsesAnalysisManagers()==false` | Uses the bounded worker pool |
| `Isolated` | Verified module in a clean worker process | Uses the bounded worker pool |

The scheduler is completion-driven: a dependent can start as soon as its own
dependencies finish. `CASCADE_DAG_MAX_WORKERS` bounds the total active `Root`,
`Parallel`, and `Isolated` work for one DAG. A ready `Serial` node acts as a barrier:
the scheduler stops dispatching other ready pooled nodes, drains active work, and
runs the serial node. This favors deterministic exclusive work over maximum pool
utilization.

## Isolated-worker boundary

An isolated run starts a clean executable with `exec()`. The worker:

- receives the selected package and module identity rather than scanning every
  plugin root;
- reopens the manifest and revalidates the requested artifact and trust policy;
- rejects relative, missing, foreign-owned, or group/world-writable worker files;
- rejects worker and Python-runtime paths below replaceable group/world-writable
  parent directories; sticky exceptions must be owned by root or the current user;
- closes inherited file descriptors except its explicit protocol descriptors;
- removes loader-injection and Python startup environment variables;
- enables Linux `no_new_privs` and a private umask;
- starts Python with interpreter isolation and a canonical runtime directory;
- returns one bounded serialized `RunResult` to the parent.

Optional limits cap address space, output file size, child process count, and open
files. They are disabled by default because safe values depend on the analysis.
Set an explicit timeout for unattended workflows. A timeout or cancellation sends
`SIGTERM`, followed by `SIGKILL` if the child does not exit.

Native library constructors execute when a verified C++ plugin is loaded. ABI and
registration entry points are checked around loading, but package trust remains the
security boundary: neither signed packages nor isolated workers make arbitrary
native code harmless.

## Runtime variables

Values are read from the process environment. Configure them before constructing
controllers or starting concurrent work; changing process-wide environment state
during active runs is unsupported.

| Variable | Default | Meaning |
| --- | --- | --- |
| `CASCADE_OUTPUT_DIR` | Current working directory | Construction-time module output root |
| `CASCADE_CACHE_DIR` | `~/.cache/cascade/snapshot_cache` | Snapshot cache and failed/skipped provenance root |
| `CASCADE_INPUT_HASH_MODE` | `metadata` | `metadata`, `auto`, or `full` tracked-input identity |
| `CASCADE_PROVENANCE_HASH_MODE` | `full` | `full`, `metadata`, or `none` output artifact hashing |
| `CASCADE_PROVENANCE_HASH_CACHE_ENTRIES` | `1024` | Process-local full-hash cache bound; `0` disables it |
| `CASCADE_CACHE_MAX_SNAPSHOTS` | `256` | Snapshot history retained per module; `0` is unlimited |
| `CASCADE_DAG_MAX_WORKERS` | Hardware concurrency | Positive pooled DAG concurrency bound |
| `CASCADE_PROGRESS_INTERVAL_MS` | `200` | Non-negative terminal-render interval; `0` renders every update |
| `CASCADE_ISOLATED_TIMEOUT_SECONDS` | `0` | Non-negative worker deadline; `0` disables it |
| `CASCADE_WORKER_MEMORY_LIMIT_MB` | Unset | Positive isolated-worker address-space limit |
| `CASCADE_WORKER_FILE_SIZE_LIMIT_MB` | Unset | Positive isolated-worker file-size limit |
| `CASCADE_WORKER_MAX_PROCESSES` | Unset | Positive isolated-worker process-count limit |
| `CASCADE_WORKER_MAX_OPEN_FILES` | Unset | Positive isolated-worker descriptor limit |
| `CASCADE_PREFIX` | Active installation prefix | Runtime, plugin, and trust-store base |
| `CASCADE_CONFIG_FILE` | XDG Cascade config | Persistent plugin-prefix configuration path |
| `CASCADE_PLUGIN_DIR` | Prefix-derived | Temporary C++ plugin-root override |
| `CASCADE_PYPLUGIN_DIR` | Prefix-derived | Temporary Python plugin-root override |
| `CASCADE_PLUGIN_TRUST_STORE` | Prefix-derived | Temporary trusted-key root override |
| `CASCADE_PYTHON_RUNTIME_DIR` | `${CASCADE_PREFIX}/lib` | Canonical Python parent imported by an isolated worker |

`CASCADE_CPP_WORKER` and `CASCADE_PYTHON_WORKER` override worker executable paths.
They exist for installation layouts, packaging tests, and diagnostics. Both must be
absolute, executable, owner/root controlled, and not group/world writable; normal
installations should use the prefix-derived executables. Every parent directory
must also be protected from group/world replacement, except sticky directories
owned by root or the current user, such as `/tmp`. Run `cascade doctor runtime` to
inspect the resolved paths and all runtime policy values before an unattended
workflow.

## Suggested profiles

### Interactive analysis with large ROOT inputs

```bash
export CASCADE_INPUT_HASH_MODE=metadata
export CASCADE_PROVENANCE_HASH_MODE=full
export CASCADE_DAG_MAX_WORKERS=4
```

This avoids a complete input read while keeping strong validation for produced
artifacts. Tune worker count to memory pressure, not just CPU count.

### Reproducible release or archival run

```bash
export CASCADE_INPUT_HASH_MODE=full
export CASCADE_PROVENANCE_HASH_MODE=full
export CASCADE_ISOLATED_TIMEOUT_SECONDS=3600
```

Require signed plugins as a controller/CLI policy as well. Expect the first process
to read every tracked regular input and output once.

### Constrained isolated workers

```bash
export CASCADE_WORKER_MEMORY_LIMIT_MB=8192
export CASCADE_WORKER_FILE_SIZE_LIMIT_MB=16384
export CASCADE_WORKER_MAX_PROCESSES=16
export CASCADE_WORKER_MAX_OPEN_FILES=1024
export CASCADE_ISOLATED_TIMEOUT_SECONDS=1800
```

These are examples, not universal safe defaults. ROOT memory mapping, subprocesses,
and large output files may require higher values.

## Measuring before tuning

Distinguish these costs in measurements:

- manifest indexing at controller startup and targeted plugin verification at registration;
- tracked-input capture during snapshot construction;
- user `Init`/`Execute`/`Finalize` time;
- output hashing and directory enumeration during commit;
- output promotion and filesystem synchronization;
- isolated-worker startup and plugin reload;
- ROOT serialization enforced by the process-wide lane.

Metadata input mode only removes content reads for tracked regular files. It does
not accelerate the module's own ROOT I/O, directory enumeration, plugin artifact
verification, or full output provenance hashing.
