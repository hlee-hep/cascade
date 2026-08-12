# Performance testbench

Cascade keeps performance measurements separate from correctness tests. `scons
bench -j2` runs the DAG scheduler plus input- and output-hashing testbenches.

## DAG scheduler

The DAG testbench compares four execution shapes with the same synthetic
workload. Its default node-count sweep is `1,10,100,1000`:

| Scenario | Shape | Expected limiter |
| --- | --- | --- |
| `serial` | Independent nodes on the serial lane | Serial lane |
| `parallel` | Independent nodes on the parallel lane | Worker pool or host |
| `root` | Independent nodes on the ROOT lane | Global ROOT mutex |
| `chain` | Parallel-lane nodes in one dependency chain | Critical path |

Build and run the default matrix:

```bash
scons bench -j2
```

The executable can also be run directly with controlled inputs. Set non-zero
work to measure workload scaling instead of pure scheduler overhead:

```bash
build/benchmarks/cascade-dag-bench \
  --nodes 1,10,100,1000 \
  --iterations 9 \
  --work-ms 5 \
  --workers 1,2,4,8
```

Use `--json` for machine-readable output. Each row reports median and p95 wall
time, throughput, a topology-derived lower bound, and `overhead_ratio = median /
lower_bound`. A rising ratio as workers or nodes are added points to scheduler,
synchronization, or host contention.

```bash
build/benchmarks/cascade-dag-bench --json > dag-benchmark.json
```

## Input hashing

The hashing testbench compares `metadata`, `auto`, and `full` using a generated
128 MiB `.root`-shaped binary fixture. The bytes do not need to contain ROOT
objects because the provenance SHA-256 path treats every regular file equally.
The process-local digest cache is disabled, so every `full` sample reads and
hashes the entire file. `first_ms` is the first process access while
`warm_median_ms` reflects subsequent reads. OS page-cache state is deliberately
not manipulated; the generated fixture will normally already be page-cache hot.

Run it against an actual large ROOT file for a storage-representative result:

```bash
build/benchmarks/cascade-input-hash-bench \
  --input /data/events.root \
  --iterations 9
```

Machine-readable output and a different generated size are also supported:

```bash
build/benchmarks/cascade-input-hash-bench \
  --fixture-mib 1024 \
  --json > input-hash-benchmark.json
```

For files larger than 64 MiB, `auto` should resolve to `metadata`; `full` exposes
the actual sequential read plus SHA-256 cost. Filesystem page-cache state still
matters, so use the actual data volume and storage when making a policy decision.

## Output hashing

The output benchmark invokes the artifact-capture path used by
`ProvenanceRecorder::BuildModuleRun` and compares `none`, `metadata`, and `full`.
It does not write an additional provenance file. As with the input benchmark,
the digest cache is disabled and OS page-cache state is left unchanged.

Run the generated 128 MiB fixture through all output policies:

```bash
build/benchmarks/cascade-output-hash-bench
```

Measure an actual produced ROOT file without modifying it:

```bash
build/benchmarks/cascade-output-hash-bench \
  --output /data/results.root \
  --iterations 9
```

Use `--fixture-mib N` to change the generated size and `--json` for
machine-readable results.

Run comparisons on the same machine, power policy, build, node count, and
workload. Prefer at least nine iterations for results used in a performance
decision. This is a diagnostic benchmark, not a correctness gate, so timing
thresholds are deliberately not part of `scons verify`.

## CI baseline artifacts

Every push to `main` and every pull request runs the three testbenches with
machine-readable output. GitHub Actions uploads `dag.json`, `input-hash.json`,
`output-hash.json`, and `metadata.json` as
`cascade-benchmark-<commit-sha>`. Metadata records the commit, runner OS, CPU,
architecture, kernel, ROOT, compiler, and Python versions.

These artifacts are an observation history, not a performance gate. Compare
results only across sufficiently similar runners and storage environments. Each
release candidate also attaches the four JSON files to its GitHub prerelease so
the baseline outlives the workflow artifact retention period.
