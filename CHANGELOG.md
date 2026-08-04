# Changelog

All notable user-visible changes to Cascade are recorded here. The format is
based on Keep a Changelog, and releases follow Semantic Versioning.

## [Unreleased]

### Added

- Snapshot cache inspection, hit/miss explanation, and locked pruning through the CLI.
- Declarative DAG validation without module execution.
- Release preparation checklist and operational verification guidance.
- MIT License for source and distribution terms.

## [0.3.0] - Unreleased

### Added

- Initial public C++ plugin ABI 1 with full build fingerprint checks.
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
