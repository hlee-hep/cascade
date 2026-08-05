# Changelog

All notable user-visible changes to Cascade are recorded here. The format is
based on Keep a Changelog, and releases follow Semantic Versioning.

## [Unreleased]

### Added

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

### Changed

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

## [0.3.0] - Unreleased

### Added

- Initial public C++ plugin ABI 3 with full build fingerprint checks.
- Verified C++ and Python plugin packages with optional Ed25519 publisher signatures.
- Transactional plugin installation and persistent plugin prefix discovery.
- Unified C++/Python module lifecycle, output transactions, cancellation, and subprocess isolation.
- Deterministic DAG execution with parameter links and failure propagation.
- Versioned module/workflow provenance, run history, inspection, comparison, and replay.
- Schema-validated ROOT input, cut, and histogram configuration.

### Changed

- The supported Python control surface is `py_amcm`; raw bindings are internal integration surfaces.
- Analysis configuration documents require `schema_version: 1`.
- Snapshot caches use schema 1 entries linked to provenance manifests while retaining legacy hash-only reads.

### Security

- Plugin discovery verifies package boundaries, regular files, hashes, ABI metadata, and the configured signature policy before registration.
- Protected outputs are staged and promoted only after successful lifecycle completion.

[Unreleased]: https://github.com/hlee-hep/cascade/compare/v0.3.0...HEAD
[0.3.0]: https://github.com/hlee-hep/cascade/releases/tag/v0.3.0
