# Build and installation

## Requirements

Cascade is built with SCons and expects dependency discovery through command-line
tools:

| Dependency | Probe |
| --- | --- |
| ROOT | `root-config --cflags --libs` |
| yaml-cpp | `pkg-config --cflags --libs yaml-cpp` |
| pybind11 | `python3 -m pybind11 --includes` |
| PyYAML | `python3 -c 'import yaml; print(yaml.__version__)'` |
| OpenSSL | headers and `ssl`, `crypto` libraries |
| nlohmann/json | headers available to the compiler |
| Operating system | Linux |
| C++ compiler | Support for ROOT's configured C++17, C++20, or C++23 mode |

Cascade reads the `-std=` mode and version from `root-config`, uses that mode for
the framework, and installs `CascadeBuildConfig.hh` for plugin builds. The runtime
and plugins must use ABI-compatible compiler, standard-library, ROOT,
pointer-width, language-standard, and build-mode settings.

## SCons targets

```bash
scons -j2          # Build core libraries and Python binding
scons test -j2     # Build and run C++ and Python tests
scons verify -j2   # Run the complete local release gate
scons install      # Install to the configured prefix
scons compdb       # Generate compile_commands.json
scons tidy         # Run clang-tidy over framework sources (requires clang-tidy)
scons -c           # Remove SCons build products
```

Use `verify` before installation or release. It depends on `test`, compiles a
native plugin fixture using no ROOT include/link flags, checks whitespace, and
runs `doctor runtime` plus manifest/hash/ABI verification against the built test
package. Plugin smoke tests should still be run against the exact installed prefix
intended for use.

## Install variables

| Variable | Default |
| --- | --- |
| `PREFIX` | `~/.local` |
| `LIBDIR` | `${PREFIX}/lib` |
| `BINDIR` | `${PREFIX}/bin` |
| `INCLUDEDIR` | `${PREFIX}/include/cascade` |
| `PYTHONDIR` | `${LIBDIR}/cascade` |
| `PYMODULEDIR` | `${PYTHONDIR}/pymodule` |

Example:

```bash
scons install \
  PREFIX=/opt/cascade \
  LIBDIR=/opt/cascade/lib \
  BINDIR=/opt/cascade/bin \
  INCLUDEDIR=/opt/cascade/include/cascade
```

Unset component variables inherit from `PREFIX`.

## Installed layout

```text
${PREFIX}/
  bin/
    cascade
  include/cascade/
    IAnalysisModule.hh
    AnalysisManager.hh
    ...
  lib/
    libCascade.so
    libAMCM.so
    libAnalysisManager.so
    libParamManager.so
    libPlotManager.so
    libutils.so
    cascade/
      __init__.py
      _cascade.so
      py_amcm.py
      pymodule/
        base_module.py
      plugin/
      pyplugin/
  share/cascade/
    scripts/
      plugin_sconstruct
      sign_plugin.sh
    trusted_keys/
```

ROOT dictionary `.pcm` and `.rootmap` files are installed beside their libraries.

Release builds enable stack-protector and fortified libc checks and link with
RELRO, immediate symbol binding, and a non-executable stack. The native isolated
worker is also built as PIE.

On Linux, verify release hardening from the installed artifacts rather than relying
only on build flags:

```bash
readelf -h "${CASCADE_PREFIX}/bin/cascade-worker" | grep 'Type:'
readelf -lW "${CASCADE_PREFIX}/bin/cascade-worker" | grep GNU_STACK
readelf -lW "${CASCADE_PREFIX}/lib/libCascade.so" | grep GNU_RELRO
readelf -dW "${CASCADE_PREFIX}/lib/libCascade.so" | grep -E 'BIND_NOW|FLAGS_1'
```

The worker should be a position-independent executable, `GNU_STACK` should not be
executable, and the library should report RELRO plus immediate binding. Tool output
varies by binutils release.

## Runtime environment

For a non-system prefix:

```bash
export CASCADE_PREFIX=/your/cascade/prefix
export PATH="${CASCADE_PREFIX}/bin:${PATH}"
export PYTHONPATH="${CASCADE_PREFIX}/lib:${PYTHONPATH}"
export LD_LIBRARY_PATH="${CASCADE_PREFIX}/lib:${LD_LIBRARY_PATH}"
```

Plugin locations derive from the prefix:

```text
${CASCADE_PREFIX}/lib/cascade/plugin
${CASCADE_PREFIX}/lib/cascade/pyplugin
${CASCADE_PREFIX}/share/cascade/trusted_keys
```

Additional plugin installations should normally be registered persistently:

```bash
cascade plugin install /path/to/plugin --prefix /data/cascade-plugins
cascade plugin path list
```

Registered prefixes survive new terminal sessions. Cascade still validates
their packages on every process start.

Environment overrides remain useful for temporary tests:

```bash
export CASCADE_PLUGIN_DIR=/path/to/cpp/packages
export CASCADE_PYPLUGIN_DIR=/path/to/python/packages
export CASCADE_PLUGIN_TRUST_STORE=/path/to/trusted_keys
```

Other runtime directories:

| Variable | Purpose |
| --- | --- |
| `CASCADE_OUTPUT_DIR` | Default module output root |
| `CASCADE_CACHE_DIR` | Default snapshot-cache root |

Per-module setters take precedence for output/cache placement after construction.
See [Runtime reliability and performance](runtime-reference.md#runtime-variables)
for hashing, cache retention, DAG, progress, isolation, and worker-limit controls.

## Verify an installation

```bash
python3 -c 'import cascade; print(cascade.__version__); print(cascade.__abi_tag__)'
cascade doctor plugins
```

For a new empty prefix, `doctor plugins` may report missing plugin roots. A trust
store is required only by `cascade --require-signed ...`.

Check direct dynamic linking when imports fail:

```bash
ldd "${CASCADE_PREFIX}/lib/libCascade.so"
ldd "${CASCADE_PREFIX}/lib/cascade/_cascade.so"
```

## Development prefix

Using a dedicated prefix avoids mixing installed releases:

```bash
scons install PREFIX=/path/to/work/cascade-install
```

Build external plugins with the same `CASCADE_PREFIX`. After any ABI-visible
framework change:

1. reinstall Cascade;
2. rebuild all C++ plugin libraries;
3. regenerate plugin manifests and re-sign distributed packages;
4. run `cascade doctor plugins`;
5. run normal and isolated smoke tests.

## Release verification

Before preparing a release, run locally in the intended ROOT and compiler
environment:

```bash
scons verify -j2
```

For plugin changes, also install into a temporary prefix, build a verified test
package, run `cascade doctor plugins`, and execute one in-process and one isolated
workflow. Signed distributions should additionally build a signed package and run
`cascade --require-signed doctor plugins`.
