# Versioning and compatibility

Cascade tracks project releases and C++ plugin compatibility separately.

| Source line | Framework version | C++ plugin ABI |
| --- | --- | --- |
| Current development tree | `0.4.0-dev` | 4 |
| Latest release | `0.3.0` | 3 |

`include/Version.hh` is the single source of truth for the framework version;
`include/PluginABI.hh` defines the plugin ABI. Python, CLI, and provenance
obtain the framework version from the C++ runtime. CI checks the installed runtime
against these headers with `scripts/check_runtime_version.py`; release CI also
checks the Git tag. Update current-version examples when changing the headers,
and preserve historical release notes.

## Semantic version

The project version is `MAJOR.MINOR.PATCH`, optionally followed by a SemVer
prerelease suffix such as `-rc1`:

- `cascade.__version__`;
- `CascadeVersionString()` in `include/Version.hh`.

Before 1.0, a minor release may intentionally break public API or ABI when the
change is documented.

## Plugin ABI version

The integer ABI changes when public C++ binary compatibility can break:

- virtual interface or class-layout changes;
- exported signature changes;
- ownership/exception contract changes affecting binary callers.

Runtime access:

```python
import cascade
print(cascade.__abi_version__)
```

C++ definition:

```cpp
CASCADE_PLUGIN_ABI_VERSION
```

The 0.4.0-dev native ROOT transition uses ABI 4. It removes the AnalysisManager API and
renames the ROOT scheduling hook to `UsesRoot`. Rebuild C++ plugins against the
new headers and libraries. Cascade 0.3 used ABI 3; ABI 1 and 2 were pre-release.

## ABI fingerprint

The integer alone cannot detect toolchain incompatibility. ABI 4 also compares:

- compiler family and exact version;
- `__cplusplus`;
- standard library and version;
- libstdc++ C++11 ABI setting;
- ROOT version;
- pointer width;
- debug/release mode;
- libstdc++ debug mode.

```python
print(cascade.__abi_tag__)
```

Plugins with equal integer ABI but unequal tags are rejected before registration.

The semantic version is deliberately absent from the ABI tag. A patch/minor release
can remain binary-compatible when the public ABI and build fingerprint are stable.

## Plugin manifest version

Plugin distribution currently writes manifest schema 3 and continues to read
schema 2 packages. Manifest schema is separate from C++ ABI and analysis
configuration schema.

## Analysis configuration version

Cut and histogram YAML documents currently use `schema_version: 1`.
Input YAML configuration was removed in 0.4.0-dev.
Parameter files use their registered parameter contract rather than a document
schema version.

## Provenance schema version

Module and workflow manifests use separate document identities,
`cascade.module-run` and `cascade.workflow-run`, both at `schema_version: 1`.
Snapshot cache schema 1 links hashes to module manifests. These document versions
are independent of plugin ABI 4. Snapshot identity hashing uses schema 5; this
is distinct from the schema 1 cache document that stores those identities.

## Release policy

| Change | Semantic bump | ABI bump |
| --- | --- | --- |
| Documentation/test-only | Patch as appropriate | No |
| Internal implementation, unchanged public binary surface | Patch/minor | No |
| New backward-compatible API | Minor | Usually no |
| Public class layout/virtual/signature break | Minor before 1.0, major after | Yes |
| Plugin manifest format break | Minor/major | Not necessarily |
| Analysis config semantic break | Minor/major and schema bump | Not necessarily |

## Plugin rebuild rule

Rebuild and regenerate the manifest when:

- `cascade.__abi_version__` changes;
- `cascade.__abi_tag__` changes;
- plugin source or build flags change;
- linked ROOT/toolchain changes;
- any installed plugin file changes.

Re-sign when the package is distributed under the signed policy. Use
`cascade doctor plugins` as the normal gate and
`cascade --require-signed doctor plugins` as the signed-distribution gate.
