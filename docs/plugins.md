Plugin ABI and Development Guide

ABI Versioning Policy
- Bump `CASCADE_PLUGIN_ABI_VERSION` when you change any ABI-visible surface (class layout, virtuals, or signatures).
- Do not bump for internal-only changes that do not affect plugin headers or binary compatibility.
- If an old plugin can fail to load, link, or run with a new `libCascade`, bump the ABI.
The runtime ABI version is available as `cascade.__abi_version__` in Python.

Required ABI Entry Points
Plugins must export two C symbols:

```cpp
#include "PluginABI.hh"
#include "MyModule.hh"

CASCADE_PLUGIN_EXPORT int CascadePluginAbiVersion() { return CASCADE_PLUGIN_ABI_VERSION; }
CASCADE_PLUGIN_EXPORT void CascadeRegisterPlugin() { CASCADE_REGISTER_MODULE(MyModule); }
```

Build Requirements
- Compile against the headers from the target Cascade installation.
- Link against the same `libCascade` you will run with.
- Avoid inline/template-heavy APIs across the plugin boundary.

Recommended Environment Variables
- `CASCADE_PREFIX` (Cascade install root)
- `CASCADE_CORE_INCLUDE` (defaults to `${CASCADE_PREFIX}/include/cascade`)
- `CASCADE_CORE_LIB` (defaults to `${CASCADE_PREFIX}/lib`)
- `CASCADE_PLUGIN_LIB_DIR` (defaults to `${CASCADE_PREFIX}/lib/cascade/plugin`)

Distribution
- Distribute the built `.so` together with the ABI version used.
- Rebuild plugins if the ABI version changes.
