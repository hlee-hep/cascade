# Plugin development and distribution

Cascade core contains no analysis modules. C++ and Python modules are discovered
from verified plugin packages. Publisher signatures are optional and can be
required for distributed or managed installations.

Plugin root discovery, persistent prefix configuration, filesystem validation,
hashing, manifest verification, trust decisions, and index locking are implemented
once in the C++ core. The Python API and CLI keep their existing function names but
delegate these operations to the same services. Python retains only the language
boundary needed to compile/import a verified Python artifact and inspect its class
metadata.

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

The recommended workflow is the transactional CLI installer:

```bash
cascade plugin install . --prefix ~/.local
```

It uses the active Cascade installation as the build SDK, installs into a
temporary directory below the destination prefix, verifies the staged package,
publishes it, and persistently registers the prefix. A new terminal therefore
discovers the package without plugin-root environment variables.

Use signing keys when building a signed distribution:

```bash
cascade --require-signed plugin install . \
  --prefix /opt/experiment-plugins \
  --private-key /secure/path/plugin_private.pem \
  --public-key /provisioning/path/plugin_public.pem
```

The low-level SCons workflow remains available for unsigned packaging and development.

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

`CASCADE_PLUGIN_PACKAGE` accepts letters, digits, `.`, `_`, and `-`, and must start
with a letter or digit.

Signed installation must use `cascade plugin install` with both `--private-key`
and `--public-key`. Cascade signs the completed staging tree after the plugin build
process exits, so plugin-controlled build code never receives the private-key path.
The trusted key is installed as `<package>.pem`; a key trusted for one package
cannot authorize another package.

To install plugins outside the core Cascade prefix with low-level SCons, keep
the SDK and destination concepts separate:

```bash
CASCADE_PREFIX=/opt/cascade \
CASCADE_PLUGIN_PREFIX=/data/cascade-plugins \
CASCADE_PLUGIN_PACKAGE=my_package \
scons install

cascade plugin path add /data/cascade-plugins
```

`CASCADE_PREFIX` locates headers, libraries, and the installed SCons template.
`CASCADE_PLUGIN_PREFIX` controls only the plugin destination.

## Persistent plugin prefixes

Cascade stores paths, not discovery results, in:

```text
${XDG_CONFIG_HOME:-~/.config}/cascade/config.json
```

Schema 1 is:

```json
{
  "schema": 1,
  "plugin_prefixes": [
    {"path": "/data/cascade-plugins", "enabled": true}
  ]
}
```

Each process scans the configured prefixes and repeats the normal manifest,
artifact, ABI, and signature validation. The configuration is not a trusted
module cache. `CASCADE_PLUGIN_DIR`, `CASCADE_PYPLUGIN_DIR`, and
`CASCADE_PLUGIN_TRUST_STORE` remain temporary compatibility overrides.

Normal controller startup performs this full discovery. An isolated worker receives
the already selected manifest path and identity, then revalidates only that package
and requested artifact before loading it. Isolation therefore keeps the trust check
without paying a full-prefix scan for every node.

Unsigned packages accepted by the default `Verified` policy must be owned by the
current user or root and their plugin root, package directory, manifest, and selected
artifact must not be group/world writable. Signed packages derive trust from the
verified manifest signature. Runtime reads are bounded to 4 MiB for manifests,
1 MiB for public keys/signatures, and 64 MiB for Python source artifacts.

Python discovery caches a verified index per controller. Metadata listing parses
literal `METADATA`, `VERSION`, `SUMMARY`, and `TAGS` class constants without
executing plugin source; pass the explicit instantiation option only when dynamic
metadata is required.

Install or remove packages before constructing a long-lived controller. Its Python
index remains stable for the controller lifetime, so a newly published
Python package is visible to a new controller/process rather than appearing midway
through an active workflow.

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

If a signature is present, it must be valid under the package-bound
`<package>.pem` key even with the default policy. `RequireSigned` additionally
rejects packages that have no signature. Changing any installed file invalidates
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

Under the default policy, unsigned packages may load as `VERIFIED`. Invalid or
untrusted signatures are always rejected; there is no downgrade to unsigned
trust. Under `RequireSigned`, missing signatures are rejected as well.

## ABI 2

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

## Refreshing discovery in a long-running process

Controllers discover plugins when they are constructed. A notebook, service, or
interactive Python process can discover packages installed afterward without
restarting:

```python
changes = controller.refresh_plugins()
print(changes["added_cpp"])
print(changes["added_python"])
```

Refresh is allowed only while the DAG is idle. New C++ libraries are verified and
loaded, and new or removed Python declarations update the controller's discovery
index. Already loaded native libraries and imported Python source cannot be safely
replaced or unloaded in place. If an installed artifact at an existing path changed,
refresh raises an error naming the affected plugin and requires a new process.

C++ callers can use `AMCM::RefreshPlugins()`, which returns newly available C++
module names. C++ plugins removed from disk remain loaded until the process exits;
the same lifetime rule applies to already registered module instances.

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

The command reports each package as `VERIFIED` or `SIGNED`, then performs the
same static manifest, signature, boundary, hash, and identity validation used by
both runtimes. It does not `dlopen` libraries merely for diagnosis, avoiding
execution of plugin constructors; C++ ABI checks still run in the actual loader.
`--require-signed` turns every unsigned package into an error.

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
plugin code safe. C++ plugins execute with the process's privileges; the clean
`exec()` worker avoids inherited runtime locks, applies resource-limit hooks and
`no_new_privs`, and contains crashes, but it is not a filesystem or network sandbox.

The practical trust boundaries are:

| Control | Protects against | Does not protect against |
| --- | --- | --- |
| Manifest hash and package boundary checks | Accidental replacement, path escape, wrong artifact | A malicious package author |
| Unsigned ownership/mode checks | Other local users modifying an accepted package through common writable paths | Code intentionally installed by the current user/root |
| Publisher signature | Modification after signing and an untrusted publisher under `RequireSigned` | Harmful behavior signed by a trusted publisher |
| ABI version and tag | Known compiler/ROOT/standard-library incompatibility | Logic bugs or arbitrary native constructor behavior |
| Isolated worker | Fatal signals, inherited locks/state, bounded optional resources | Ordinary filesystem/network access or same-user data theft |

Loading a C++ shared library may execute native constructors before registration is
complete. Use `cascade doctor plugins` for static package diagnosis without
`dlopen`, require signatures in managed deployments, and isolate risky data or
native code. Do not treat `Verified` unsigned packages as third-party sandboxed
extensions.

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
