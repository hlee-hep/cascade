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

CASCADE_PLUGIN_EXPORT_ABI
CASCADE_PLUGIN_EXPORT void CascadeRegisterPlugin() { CASCADE_REGISTER_MODULE(MyModule); }
```

Build Requirements
- Compile against the headers from the target Cascade installation.
- Link against the same `libCascade` you will run with.
- Avoid inline/template-heavy APIs across the plugin boundary.
- Define `CASCADE_PLUGIN_NO_AUTO_REGISTER` so plugins only register via `CascadeRegisterPlugin()`.

Recommended Environment Variables
- `CASCADE_PREFIX` (Cascade install root)
- `CASCADE_CORE_INCLUDE` (defaults to `${CASCADE_PREFIX}/include/cascade`)
- `CASCADE_CORE_LIB` (defaults to `${CASCADE_PREFIX}/lib`)
- `CASCADE_PLUGIN_LIB_DIR` (defaults to `${CASCADE_PREFIX}/lib/cascade/plugin`)

Distribution
- Distribute the built `.so` together with the ABI version used.
- Rebuild plugins if the ABI version changes.

Signature Verification (optional)
- Place an Ed25519 public key at `plugin_pubkey.pem` in the plugin directory.
- Each plugin must have a detached signature: `libMyModule.so.sig` (raw signature bytes).
- If the key is missing, plugins are skipped.
- If the key is present, unsigned or invalid plugins are skipped.
Python plugins follow the same rule using `plugin_pubkey.pem` in the Python plugin directory and `module.py.sig` signatures.

Signing Helper
Use `scripts/sign_plugin.sh` to create a `.sig` file from a private key:

```bash
./scripts/sign_plugin.sh /path/to/libMyModule.so /path/to/private_key.pem
```

Examples

Generate a keypair (Ed25519):

```bash
openssl genpkey -algorithm ED25519 -out plugin_private.pem
openssl pkey -in plugin_private.pem -pubout -out plugin_pubkey.pem
```

Sign a plugin and verify loading:

```bash
cp plugin_pubkey.pem /home/$USER/.local/lib/cascade/plugin/plugin_pubkey.pem
./scripts/sign_plugin.sh /home/$USER/.local/lib/cascade/plugin/libMyModule.so plugin_private.pem
```
