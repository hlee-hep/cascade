# Plugin development and distribution

Cascade core contains no analysis modules. C++ and Python module candidates are
indexed from package manifests, then the selected package is fully verified when
a module is registered. Publisher signatures are optional and can be required for
distributed or managed installations.

Plugin root discovery, persistent prefix configuration, filesystem validation,
manifest indexing, hashing, verification, trust decisions, and index locking are implemented
once in the C++ core. The Python API and CLI keep their existing function names but
delegate these operations to the same services. Python retains only the language
boundary needed to compile/import a selected, verified Python artifact.

## Source layout

The CLI recognizes this conventional source layout:

```text
my_package/
  cascade-plugin.yaml  # optional
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
- C++ class names match header stems unless `class_map` says otherwise;
- module class names are globally unique across installed C++ and Python packages.

No build file is required. `cascade plugin install` invokes Cascade's installed
plugin build template directly and supplies the active SDK and staging paths.

The optional `cascade-plugin.yaml` declares only departures from the ROOT-free,
stem-matched defaults:

```yaml
schema_version: 1
root_modules:
  - RootEventModule
class_map:
  EventModule: experiment::EventModule
metadata:
  experiment::EventModule:
    version: 1.0.0
    summary: Produces the experiment event sample.
    tags: [generator, root]
```

Omitting `root_modules` keeps every C++ module ROOT-free. Use `['*']` only when
every module in the package needs ROOT. List only modules that include ROOT or
use `AnalysisManager`/`PlotManager`. The build template reads the installed
`CascadeBuildConfig.hh`, applies the same C++ language mode as Cascade, and rejects
a different ROOT version or C++ mode for ROOT-using modules.

`metadata` is copied into the installed manifest. It lets `cascade module list`
show C++ versions, summaries, and tags without loading a shared library. Python
`VERSION`, `SUMMARY`, `TAGS`, or literal `METADATA` class attributes are extracted
into the manifest automatically; an explicit configuration entry can override
them.

## Verified module identity

Plugin sources are compiled or installed without text substitution. After a
verified artifact is selected, the loader assigns:

- the manifest's C++ registration name or Python class name as the module basename;
- the verified artifact SHA-256 as the module code hash.

Constructors only register analysis parameters and state. They do not call
`SetBaseName`/`SetCodeHash` or assign Python identity fields. This keeps source
files directly compilable and makes plugin verification, cache identity, worker
selection, and provenance use the same artifact identity.

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

Provide the function yourself only when custom registration is needed. All three
entry points are required:

- `CascadePluginAbiVersion`;
- `CascadePluginAbiTag`;
- `CascadeRegisterPlugin`.

Runtime metadata returned by `GetMetadata()` remains available after registration.
For metadata listing before registration, declare the same public description in
`cascade-plugin.yaml`. Do not use static registration in a plugin: registration
must happen only after the loader verifies the package manifest, artifact hash,
ABI, and ABI tag.

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

The installer copies these static class attributes into the package manifest.
Metadata listing therefore does not parse or import installed Python source. The
package loader imports only a selected, verified file into a private
`cascade.pyplugin` namespace.

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

Package-owned `SConstruct` files and low-level plugin SCons installation are not
supported. The CLI is the single build/install entry point. For signed packages,
it signs the completed staging tree after the build process exits, so
plugin-controlled code never receives the private-key path. The trusted key is
installed as `<package>.pem`; a key trusted for one package cannot authorize
another package.

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

Normal controller startup reads bounded `plugin_manifest.json` files only. It does
not hash artifacts, verify signatures, import Python, or `dlopen` C++ libraries.
`register_module()` resolves the requested identity through that lightweight index,
then verifies only its package and selected artifact before loading it. An isolated
worker receives the selected manifest path and identity and repeats that targeted
verification before execution.

Unsigned packages accepted by the default `Verified` policy must be owned by the
current user or root and their plugin root, package directory, manifest, and selected
artifact must not be group/world writable. Signed packages derive trust from the
verified manifest signature. Runtime reads are bounded to 4 MiB for manifests,
1 MiB for public keys/signatures, and 64 MiB for Python source artifacts.

Python discovery caches a manifest index per controller. Metadata listing reads
the metadata embedded by the installer. The legacy explicit instantiation option
may verify and construct a selected Python module when an older manifest lacks
metadata, but normal CLI listing never needs it.

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

## ABI 3

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

ABI 3's public `IAnalysisModule.hh` is declaration-only and ROOT-free. Plugin code
accesses parameters through `Parameters()` while the verified loader assigns
identity. Lifecycle/manager implementation is linked from `libAMCM` instead of
embedding those fields and inline methods in every plugin library.

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
