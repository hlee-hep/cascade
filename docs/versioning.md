# Versioning and compatibility

Cascade tracks project releases and C++ plugin compatibility separately.

## Semantic version

The project version is `MAJOR.MINOR.PATCH`:

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

Cascade 0.3 establishes ABI 1 as the first public plugin ABI. The number was
reset before external plugins were published.

## ABI fingerprint

The integer alone cannot detect toolchain incompatibility. ABI 1 also compares:

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

Plugin distribution currently uses manifest schema 2. Manifest schema is separate
from C++ ABI and analysis configuration schema.

## Analysis configuration version

Input, cut, and histogram YAML documents currently use `schema_version: 1`.
Parameter files use their registered parameter contract rather than a document
schema version.

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

Rebuild and re-sign when:

- `cascade.__abi_version__` changes;
- `cascade.__abi_tag__` changes;
- plugin source or build flags change;
- linked ROOT/toolchain changes;
- any installed plugin file changes.

Use `cascade doctor plugins` as the deployment gate.
