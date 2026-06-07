Build and Install Flow

Overview
- Build system: SCons
- Outputs: core shared libraries, headers, Python package, CLI, and plugin helper scripts

Key Install Variables
- `PREFIX` (default: `~/.local`)
- `LIBDIR` (default: `${PREFIX}/lib`)
- `BINDIR` (default: `${PREFIX}/bin`)
- `INCLUDEDIR` (default: `${PREFIX}/include/cascade`)
- `PYTHONDIR` (default: `${LIBDIR}/cascade`)
- `PYMODULEDIR` (default: `${PYTHONDIR}/pymodule`)
- `CASCADE_PLUGIN_DIR` (default: `${LIBDIR}/cascade/plugin`)
- `CASCADE_PYPLUGIN_DIR` (default: `${LIBDIR}/cascade/pyplugin`)

Typical Build
```bash
scons
```

Typical Install
```bash
scons install PREFIX=~/.local
```

What Gets Installed
- Core libraries: `${LIBDIR}/libCascade.so`, `libAMCM.so`, others
- Headers: `${INCLUDEDIR}/*.hh`
- Python package: `${PYTHONDIR}/*` (includes `_cascade.so` symlink)
- Python base module support: `${PYMODULEDIR}/base_module.py`
- CLI: `${BINDIR}/cascade`
- Helper scripts: `${PREFIX}/share/cascade/scripts/sign_plugin.sh`, `${PREFIX}/share/cascade/scripts/plugin_sconstruct`
- CLI diagnostics: `${BINDIR}/cascade doctor plugins`

Plugin Directories
- C++ plugins live in `${CASCADE_PLUGIN_DIR}`.
- Python plugins live in `${CASCADE_PYPLUGIN_DIR}`.
- Place `plugin_pubkey.pem`, `plugin_manifest.json`, and `plugin_manifest.json.sig` in each plugin directory.

Plugin Diagnostics
```bash
cascade doctor plugins
```

This verifies signed manifests, file hashes, stale unlisted files, Python classes, and C++ ABI compatibility.
