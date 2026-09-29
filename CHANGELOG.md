# Changelog

All notable user-visible changes to Cascade are recorded here. The format is
based on Keep a Changelog, and releases follow Semantic Versioning.

## [Unreleased]

### Changed

- Synchronize the generated test runtime with current outputs, removing retired
  modules and bytecode. Add clean deployment staging and check the staged runtime
  in release CI.
- Define the Python `Controller` type once and use the shared native parameter
  conversion for both C++ and Python module handles.
- Unify NumPy and ROOT publication rendering through `PublicationFigure`.
  Replace `plt_plot_manager` with `histogram_panel`, a NumPy input adapter with
  shared bin edges and explicit symmetric sumw2 errors.
- Remove `py_amcm` and `BelleIIPublication` class aliases; use `Controller` and
  `PublicationFigure`. Remove `SaveRunLog`, `save_run_log`, `save_run_log_all`,
  and the unused `save_provenance(dag_result=...)` argument; use
  `SaveProvenance` / `save_provenance` with an explicit path when needed.
- Replace AnalysisManager with ROOT-independent cut/histogram YAML specs and
  optional stateless native RDF helpers. Module code owns ROOT graphs, trees,
  event loops, snapshots, object lifetimes, and output actions.
- Preserve cut/histogram schema version 1; preserve YAML cut order and provide
  explicit ordered cut selection. Input YAML/automatic branch binding are removed.
- Replace `UsesAnalysisManagers` with `UsesRoot`; retain conservative ROOT
  scheduling and add explicit named module progress via `ReportProgress`.
- Track YAML/data files in module initialization and declare selection choices as
  parameters. Snapshot schema is now 5; plugin ABI is now 4. C++ plugins must be
  migrated and rebuilt. See `docs/analysis-config.md` for migration details.


## [0.3.0] - 2026-08-28

### Added

- Self-contained runtime bundle activation for co-located Python, ROOT, Cascade,
  and bundle-local plugin configuration.

### Changed

- Build configuration now accepts explicit `PYTHON` and `ROOT_CONFIG` tools and
  consistently uses their Python environment, ROOT utilities, and libraries.
- Plugin installation now defaults to the active Cascade prefix; only external
  plugin prefixes require persistent registration.

## [0.3.0-rc1] - 2026-08-12

### Added

- Deterministic DAG, input-hashing, and output-hashing bottleneck testbenches,
  including the `1,10,100,1000` node scheduler sweep.
- ASan/UBSan CI verification and deterministic randomized DAG/concurrency tests.
- Typed Python package markers and stubs for `Controller` and runtime options.
- A terminal-only Cascade banner for the no-argument command and top-level help.
- Convention-based `cascade plugin install` builds with optional
  `cascade-plugin.yaml`, removing the need for package-owned SConstruct files.
- Verified loaders now assign module basenames and code hashes directly from
  manifest identities and artifact SHA-256 values; build-time source substitution
  is removed.
- Self-contained toy dimuon plugin example covering ROOT event generation,
  RDataFrame selection, resonance fitting, reporting, cache reuse, and provenance.
- Snapshot cache inspection, hit/miss explanation, and locked pruning through the CLI.
- Per-run cache decisions and exact miss reasons in results, provenance, and CLI JSON.
- Runtime policy diagnostics, one-shot CLI tuning flags, live DAG progress, and
  long-running-process plugin refresh.
- Declarative DAG validation without module execution.
- Release preparation checklist and operational verification guidance.
- Reproducible `scons verify` gate covering tests, the ROOT-free plugin compile
  boundary, working-tree checks, runtime diagnostics, and plugin verification.
- MIT License for source and distribution terms.
- Initial public C++ plugin ABI 3 with full build fingerprint checks.
- Verified C++ and Python plugin packages with optional Ed25519 publisher signatures.
- Transactional plugin installation and persistent plugin prefix discovery.
- Unified C++/Python module lifecycle, output transactions, cancellation, and
  subprocess isolation.
- Deterministic DAG execution with parameter links and failure propagation.
- Versioned module/workflow provenance, run history, inspection, comparison, and
  replay.
- Schema-validated ROOT input, cut, and histogram configuration.

### Changed

- The supported Python control surface is now `Controller`; `AMCM` remains the
  internal Analysis Module Control Master engine name.
- Runtime execution policy is captured immutably per controller/DAG run. CLI and
  isolated Python workers pass typed options directly instead of mutating process
  environment or global state.
- Scheduler dispatch is completion-driven with a bounded worker pool, an explicit
  serial barrier, and process-wide ROOT serialization.
- AMCM isolation/provenance and provenance serialization are split into focused
  implementation units without changing the C++ plugin contract.
- Python package exports are tracked source files, and benchmark objects are built
  only below `build/benchmarks`, so clean builds no longer create source-tree
  artifacts.
- Python tests now restore injected `cascade` modules between suites, and the
  logger reevaluates terminal color support after runtime stderr redirection.
- Core, CLI, and Python-module logging now consistently uses
  `[LEVEL] [COMPONENT] message` on standard error, including per-line prefixes for
  multiline messages and shared Python module logging helpers.
- Cache output revalidation now preserves symlink identity and the output hash
  policy recorded at commit time, avoiding unnecessary full reads.
- Isolated worker and Python-runtime paths reject replaceable non-sticky writable
  parent directories, and non-finite timeout values are invalid.
- The public plugin ABI is now 3. `IAnalysisModule` stores lifecycle state behind
  a C++ implementation object, and plugins use accessor methods instead of
  embedding framework/ROOT-facing implementation state in their class layout.
- The build adopts ROOT's C++ language standard, records it in the installed SDK,
  and requires external plugins to use the same standard.
- External C++ modules are ROOT-free by default; packages explicitly list only
  module stems that need ROOT/AnalysisManager/PlotManager linkage.
- AnalysisManager and pybind registration implementations are split by feature
  without changing the public ROOT, C++, or Python APIs.
- Analysis configuration documents require `schema_version: 1`.
- Snapshot caches use schema 1 entries linked to provenance manifests while
  retaining legacy hash-only reads.

### Security

- Plugin discovery verifies package boundaries, regular files, hashes, ABI
  metadata, and the configured signature policy before registration.
- Protected outputs are staged and promoted only after successful lifecycle completion.

[Unreleased]: https://github.com/hlee-hep/cascade/compare/v0.3.0...HEAD
[0.3.0]: https://github.com/hlee-hep/cascade/releases/tag/v0.3.0
[0.3.0-rc1]: https://github.com/hlee-hep/cascade/releases/tag/v0.3.0-rc1
