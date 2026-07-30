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
| C++ compiler | C++17 support |

The runtime and plugins must use ABI-compatible compiler, standard-library, ROOT,
pointer-width, and build-mode settings.

## SCons targets

```bash
scons -j2          # Build core libraries and Python binding
scons test -j2     # Build and run C++ and Python tests
scons install      # Install to the configured prefix
scons compdb       # Generate compile_commands.json
scons tidy         # Run clang-tidy over framework sources (requires clang-tidy)
scons -c           # Remove SCons build products
```

Run tests before installation. Plugin smoke tests should be run against the exact
installed prefix intended for use.

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

Override them when testing an alternate installation:

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
| `CASCADE_RUN_LOG_DIR` | Python controller run-log directory |

Per-module setters take precedence for output/cache placement after construction.

## Verify an installation

```bash
python3 -c 'import cascade; print(cascade.__version__); print(cascade.__abi_tag__)'
cascade doctor plugins
```

For a new empty prefix, `doctor plugins` may report missing plugin roots or trusted
keys. That is expected until the first signed package is installed.

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
3. regenerate and re-sign plugin manifests;
4. run `cascade doctor plugins`;
5. run normal and isolated smoke tests.

## CI expectations

A release or pull request pipeline should minimally run:

```bash
scons test -j2
git diff --check
```

For plugin changes, also install into a temporary prefix, build a signed test
package, run `cascade doctor plugins`, and execute one in-process and one isolated
workflow.
