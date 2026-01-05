Versioning

Project Version
- Semantic versioning: MAJOR.MINOR.PATCH.
- `cascade.__version__` returns the current version string.
- C++ helpers are defined in `include/Version.hh`.

ABI Version
- `CASCADE_PLUGIN_ABI_VERSION` defines the plugin ABI version.
- `cascade.__abi_version__` returns the runtime ABI value.
- Bump the ABI when binary compatibility can break.

Suggested Bump Rules
- MAJOR: public API behavior change or ABI break.
- MINOR: new features without breaking existing code.
- PATCH: bugfixes only.
