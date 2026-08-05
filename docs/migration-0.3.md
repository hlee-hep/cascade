# Migrating to Cascade 0.3

Cascade 0.3 establishes the first public plugin contract and updates older
analysis config documents. No pre-0.3 plugin ABI is supported.

## Compatibility summary

| Area | 0.3 requirement |
| --- | --- |
| Semantic version | `0.3.0` |
| C++ plugin ABI | 2 |
| C++ standard | C++17 |
| Plugin manifest | Schema 2, verified; optional signature |
| Analysis config | `schema_version: 1` |
| Lifecycle result | `RunResult` with status, phase, message, exception |
| Protected output | `StageOutput` / `stage_output` |

## 1. Build plugins against the public ABI

ABI 2 is the initial public baseline. Rebuild any development-only binaries
against the 0.3 headers and libraries rather than treating their earlier ABI
numbers as released contracts.

The runtime now compares:

- exact compiler version;
- `__cplusplus`;
- standard-library version;
- libstdc++ C++11 ABI and debug mode;
- ROOT version;
- pointer width;
- debug/release mode.

Even ABI 2 plugins must be rebuilt when this fingerprint differs.

Check the runtime:

```python
import cascade

print(cascade.__version__)
print(cascade.__abi_version__)
print(cascade.__abi_tag__)
```

## 2. Regenerate plugin manifests

Installed files changed after rebuilding, so old SHA-256 entries and any existing
signatures are invalid. Local development requires only a regenerated manifest:

```bash
CASCADE_PLUGIN_PACKAGE=my_package scons install
cascade doctor plugins
```

For a signed distribution:

```bash
cascade --require-signed plugin install . \
  --package my_package \
  --private-key /secure/path/private.pem \
  --public-key /provisioning/path/public.pem

cascade --require-signed doctor plugins
```

Keep the private key outside the installed plugin directory and source repository.

## 3. Add analysis config schema versions

Add this document-root field to every input, cut, and histogram YAML file:

```yaml
schema_version: 1
```

Parameter YAML/JSON files do not use this field.

Run preflight or load each config in a test job. Input preflight now verifies files,
trees, scalar leaves, supported types, and explicit type matches.

## 4. Migrate output writes

Old:

```cpp
manager->WriteHistograms("results/histograms.root");
```

New:

```cpp
manager->WriteHistograms(StageOutput("histograms.root").string());
```

Old:

```python
with open("results/summary.json", "w") as output:
    json.dump(summary, output)
```

New:

```python
with self.stage_output("summary.json").open("w", encoding="utf-8") as output:
    json.dump(summary, output)
```

Configure the output root on the registered module instance:

```python
module.set_output_directory("results")
module.set_cache_directory("results/.cache")
```

Direct writes still work, but they do not roll back on lifecycle, cache, or child
process failure.

## 5. Handle `RunResult`

Do not infer success only from the module's status string:

```python
result = controller.run_module("selection")
if result.failed():
    raise RuntimeError(f"{result.phase}: {result.message}")
```

`Skipped` is normal for:

- `dry_run`;
- an already cached snapshot when `force_run` is false.

`Interrupted` is distinct from `Failed` and rolls staged output back.

## 6. Review parameter registration

External assignment now requires a pre-registered parameter with a stable type.

C++:

```cpp
m_Param.Register<double>("threshold", 25.0);
```

Python:

```python
self.register_param("threshold", 25.0)
```

Unknown names and incompatible types fail instead of silently changing the module
contract. Python parameter registration and snapshot hashing use the same C++
`ParamManager` and `SnapshotHasher` services as C++ modules, so coercion, cache
identity, and validation policy do not drift between language frontends.

Python `base_module` is also a thin subclass of the C++ `IAnalysisModule` engine.
Status transitions, `RunResult`, failure phases, cancellation, output commit,
cache updates, provenance, DAG execution, and isolated-process supervision all
follow the same C++ implementation. Python plugins provide only lifecycle
callbacks such as `init`, `execute`, `finalize`, and `snapshot_state`.

## 7. Choose an execution mode

The default remains in-process execution:

```python
controller.run_module("selection")
```

Opt in to crash containment:

```python
controller.run_module_isolated("selection")
```

Isolated execution is POSIX-only and does not copy child object state back to the
parent. Refactor downstream consumers to read committed outputs rather than member
variables when isolation is required.

## 8. Validate ownership assumptions

`AnalysisManager::RegisterTree` and `RegisterHistogram` borrow supplied ROOT
objects by default. Pass `ResourceOwnership::Owned` only when the manager should
delete them. Manager-created trees and histograms remain owned by the manager.

## 9. Replace run-log consumers with provenance

`save_run_log` and `save_run_log_all` now write workflow provenance JSON rather
than a duplicate YAML summary. Prefer `SaveProvenance` / `save_provenance`, and
read module details from the referenced `cascade.module-run` documents.

Declare material inputs with `TrackInput` / `track_input`. Transactional outputs
are discovered automatically. Legacy hash-only snapshot cache files are accepted
and upgraded to schema 1 when written.

## Migration verification

- [ ] Framework reports version 0.3.0 and ABI 2.
- [ ] No development-only binaries built against pre-baseline headers remain in active plugin roots.
- [ ] Every analysis config has `schema_version: 1`.
- [ ] Every protected output uses a staging helper.
- [ ] Every external parameter is registered.
- [ ] Material file inputs are explicitly tracked for provenance.
- [ ] Callers inspect `RunResult`.
- [ ] Plugin manifests were regenerated after the rebuild.
- [ ] Distributed signed manifests were re-signed when applicable.
- [ ] `cascade doctor plugins` reports zero errors.
- [ ] Normal, cached, failure, and isolated smoke tests pass.
