# Plugin development and distribution

Cascade core contains no analysis modules. C++ and Python modules are discovered
from verified plugin packages. Publisher signatures are optional and can be
required for distributed or managed installations.

## Source layout

The installed `plugin_sconstruct` template expects:

```text
my_package/
  SConstruct
  include/
    EventModule.hh
  src/
    EventModule.cc
  python/
    summary_module.py
```

Rules:

- every `include/*.hh` has a matching `src/*.cc`;
- a C++ file stem should end in `Module`, producing `lib...Module.so`;
- Python plugin file names end in `module.py`;
- Python plugin classes inherit `base_module`;
- C++ class names match header stems unless `CASCADE_PLUGIN_CLASS_MAP` says
  otherwise;
- module class names are globally unique across installed C++ and Python packages.

Minimal external `SConstruct`:

```python
import os

prefix = os.environ.get("CASCADE_PREFIX", os.path.expanduser("~/.local"))
template = os.path.join(prefix, "share", "cascade", "scripts", "plugin_sconstruct")

with open(template, "r", encoding="utf-8") as source:
    exec(compile(source.read(), template, "exec"), globals())
```

The in-repository mixed example loads the source-tree template directly.

## Build substitutions

The template replaces these tokens in copied build sources:

| Token | Replacement |
| --- | --- |
| `@BASENAME@` | Source file stem |
| `@VERSION_HASH@` | Hash derived from module source/header |

Typical constructor:

```cpp
EventModule::EventModule()
{
    m_Basename = "@BASENAME@";
    m_CodeVersionHash = "@VERSION_HASH@";
}
```

Python:

```python
self.basename = "@BASENAME@"
self.code_version_hash = "@VERSION_HASH@"
```

These substitutions keep the source readable while making the built module's
snapshot sensitive to code changes.

## C++ registration

When a module source does not define `CascadeRegisterPlugin`, the template
generates:

```cpp
#include "PluginABI.hh"
#include "EventModule.hh"

CASCADE_PLUGIN_EXPORT_ABI
CASCADE_PLUGIN_EXPORT void CascadeRegisterPlugin()
{
    CASCADE_REGISTER_MODULE(EventModule);
}
```

Provide the function yourself only when custom registration or static metadata is
needed. All three entry points are required:

- `CascadePluginAbiVersion`;
- `CascadePluginAbiTag`;
- `CascadeRegisterPlugin`.

`CASCADE_REGISTER_MODULE` exposes default discovery metadata containing the class
name and Cascade version. Use `CASCADE_REGISTER_MODULE_WITH_METADATA` in a custom
entry point to add a module version, summary, and tags. Do not use static
registration in a plugin: registration must happen only after the loader verifies
the package manifest, artifact hash, ABI, and ABI tag.

If the file stem and class differ:

```bash
export CASCADE_PLUGIN_CLASS_MAP='{"EventModule":"ExperimentEventModule"}'
```

The value must be valid JSON.

## Python discovery

The manifest records classes found by parsing each Python source. A discoverable
class directly inherits `base_module`:

```python
from cascade.pymodule.base_module import base_module


class SummaryModule(base_module):
    VERSION = "1.0.0"
    SUMMARY = "Builds the final analysis summary."
    TAGS = ["summary"]
```

Static class attributes allow metadata listing without constructing the module.
The package loader imports verified files into a private `cascade.pyplugin`
namespace.

Direct unindexed Python module registration is rejected. Both verified and signed
Python packages are imported through the private namespace.

## Build and install

Set the same prefix used to install Cascade:

```bash
export CASCADE_PREFIX=/your/cascade/prefix
scons -j2
```

The regular build target compiles C++ modules. A local verified installation does
not require a signing key:

```bash
CASCADE_PLUGIN_PACKAGE=my_package scons install
```

To create a signed distribution, provide a private key and optionally install its
public key into the selected prefix:

```bash
CASCADE_PLUGIN_PACKAGE=my_package \
CASCADE_PLUGIN_PRIVATE_KEY=/secure/path/plugin_private.pem \
CASCADE_PLUGIN_PUBLIC_KEY=/provisioning/path/plugin_public.pem \
scons install
```

`CASCADE_PLUGIN_PACKAGE` accepts letters, digits, `.`, `_`, and `-`, and must start
with a letter or digit.

The public key argument is optional when an operator provisions trusted keys
separately. Supplying a public key without a private key is rejected. Reinstalling
without a private key removes a stale package signature.

## Installed layout

C++ and Python artifacts have separate manifests:

```text
${CASCADE_PLUGIN_DIR}/my_package/
  libEventModule.so
  plugin_manifest.json
  plugin_manifest.json.sig  # signed distributions only

${CASCADE_PYPLUGIN_DIR}/my_package/
  __init__.py
  summary_module.py
  plugin_manifest.json
  plugin_manifest.json.sig  # signed distributions only

${CASCADE_PLUGIN_TRUST_STORE}/my_package.pem
```

Defaults:

```text
CASCADE_PLUGIN_DIR=${CASCADE_PREFIX}/lib/cascade/plugin
CASCADE_PYPLUGIN_DIR=${CASCADE_PREFIX}/lib/cascade/pyplugin
CASCADE_PLUGIN_TRUST_STORE=${CASCADE_PREFIX}/share/cascade/trusted_keys
```

Keys located inside plugin package directories are ignored. Signed trust is
granted only through the external trust store.

## Manifest schema 2

The template generates manifests; they should not be maintained by hand.

Conceptually:

```json
{
  "schema": 2,
  "package": "my_package",
  "modules": [
    {
      "name": "libEventModule",
      "language": "cpp",
      "path": "libEventModule.so",
      "sha256": "..."
    }
  ]
}
```

Validation requires:

- package name equals the containing directory;
- paths are relative and remain inside the package;
- files exist and match SHA-256;
- language-specific filename and class rules hold.

With `RequireSigned`, the manifest must additionally have a valid Ed25519
signature from the external trust store. Changing any installed file invalidates
the manifest hash and, for signed packages, requires regeneration and re-signing.

## Trust policy

The default policy is `Verified`. It enforces manifest, boundary, hash, module,
and C++ compatibility checks but does not require publisher authentication:

```cpp
AMCM controller;
```

```python
controller = py_amcm()
```

Distributed or managed workflows can require signed packages:

```cpp
AMCM controller(PluginTrustPolicy::RequireSigned);
```

```python
controller = py_amcm(require_signed=True)
```

The CLI exposes only the strengthening form:

```bash
cascade --require-signed module run EventModule
cascade --require-signed dag run workflow.yaml
```

Under the default policy, an untrusted signature is reported and the package may
still load as `VERIFIED`; it is not claimed as publisher-authenticated. Under
`RequireSigned`, missing, invalid, and untrusted signatures are rejected without
fallback.

## ABI 1

Cascade first compares the integer ABI, then the complete ABI tag. The tag covers:

- `__cplusplus`;
- compiler family and exact version;
- standard library and version;
- libstdc++ C++11 ABI mode;
- ROOT version;
- pointer width;
- debug/release mode;
- libstdc++ debug mode.

Inspect the runtime:

```python
import cascade

print(cascade.__abi_version__)
print(cascade.__abi_tag__)
```

ABI mismatch is resolved by rebuilding, not by editing the manifest.

## Diagnostics

```bash
cascade doctor plugins
cascade doctor plugins --json
cascade --require-signed doctor plugins
```

Alternate roots:

```bash
cascade doctor plugins \
  --cpp-dir /path/to/cpp/packages \
  --py-dir /path/to/python/packages \
  --trust-store /path/to/trusted_keys
```

The command reports each package as `VERIFIED` or `SIGNED`, then checks hashes,
paths, module names, Python classes, ABI version, and ABI tag. `--require-signed`
turns every unsigned or untrusted package into an error.

Useful runtime inspection:

```python
from cascade import py_amcm

controller = py_amcm()
print(controller.get_list_available_modules())
print(controller.get_list_available_module_metadata())
```

## Optional signing-key practice

- Never commit private keys.
- Keep production signing separate from developer workstations.
- Install only public keys in the runtime trust store.
- Use different keys for independent publishers or trust domains.
- Remove a compromised public key from every deployed trust store and re-sign
  trusted packages with a replacement key.

Plugin signatures establish publisher trust and file integrity. They do not make
plugin code safe. C++ plugins execute with the process's privileges; subprocess
isolation contains crashes but is not a security sandbox.

## Release checklist

- [ ] Module names and filenames follow discovery rules.
- [ ] Parameters and metadata describe the public module contract.
- [ ] C++ plugins were built against the target Cascade prefix.
- [ ] Manifests were generated after the final build.
- [ ] `cascade doctor plugins` reports zero errors.
- [ ] In-process smoke test passes.
- [ ] Isolated smoke test passes where supported.
- [ ] Package behavior is documented for users.

For a signed distribution, additionally check:

- [ ] Manifests were signed with the intended publisher key.
- [ ] Only public keys were installed in the trust store.
- [ ] `cascade --require-signed doctor plugins` reports zero errors.
