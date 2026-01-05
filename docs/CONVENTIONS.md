# Coding Conventions

These conventions match the repo's `.clang-tidy` configuration.

## Naming

- Namespaces: `CamelCase`
- Classes/structs/enums/type aliases: `CamelCase`
- Enum constants (scoped or unscoped): `CamelCase`
- Functions/methods: `CamelCase`
- Variables/parameters: `camelBack`
- Global variables: `g_` prefix + `camelBack`
- Private/protected members: `m_` prefix + `CamelCase`
- Private methods: `CamelCase` with trailing `_`
- Macros: `UPPER_CASE`

## Unused Variables

- Keep unused bindings as `_` (do not rename).
