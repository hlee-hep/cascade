# Coding conventions

These conventions apply to framework code and included examples. Existing local
style and `.clang-tidy` remain authoritative where they are more specific.

## Language

- Source identifiers, comments, diagnostics, config keys, and documentation are
  written in English.
- User-facing API names should describe intent rather than implementation details.

## C++ naming

| Construct | Style |
| --- | --- |
| Namespace | `CamelCase` |
| Class, struct, enum, type alias | `CamelCase` |
| Scoped/unscoped enum value | `CamelCase` |
| Function or method | `CamelCase` |
| Local variable or parameter | `camelBack` |
| Global variable | `g_` + `camelBack` |
| Private/protected member | `m_` + `CamelCase` |
| Private helper method | `CamelCase_` |
| Macro | `UPPER_CASE` |

Use `_` for intentionally unused structured bindings.

## C++ ownership

- Prefer RAII and standard smart pointers.
- State ownership explicitly when accepting ROOT pointers.
- `AnalysisManager::RegisterTree` and `RegisterHistogram` borrow by default.
- Use `ResourceOwnership::Owned` only for deliberate ownership transfer.
- Do not delete borrowed ROOT objects.

## Module code

- Register parameters in constructors/`__init__`.
- Keep `Init` validation cheap and deterministic.
- Keep analysis work in `Execute`.
- Use staging helpers for protected output.
- Check cancellation in long loops.
- Make snapshot-relevant inputs explicit.
- Throw actionable exceptions with the resource or parameter name.

## Python style

- Use `snake_case` for functions, methods, local variables, and module files.
- Use `CamelCase` for plugin classes.
- Use context managers for files and resources.
- Open text files with an explicit UTF-8 encoding.
- Keep imports of optional heavy dependencies inside the method that needs them.

## Diagnostics

Messages should identify:

1. subsystem/module;
2. failed operation;
3. relevant path, key, phase, or expected type;
4. original lower-level error when available.

Avoid silently returning sentinel values for new APIs when a clear exception or
`RunResult` failure is possible.

## Tests

Behavior changes should cover the relevant boundary:

- success and failure lifecycle phases;
- rollback after staged writes;
- config schema/preflight;
- cache skip and force-run behavior;
- ownership/lifetime where ROOT pointers are involved;
- isolated abnormal termination for native execution changes;
- signed package discovery for plugin changes.

Run:

```bash
scons test -j2
git diff --check
```
