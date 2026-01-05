Build and Install Flow

Overview
- Build system: SCons
- Outputs: shared libraries, headers, Python package, CLI, plugins

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
- Python modules: `${PYMODULEDIR}/*.py`
- CLI: `${BINDIR}/cascade`
- Helper scripts: `${PREFIX}/share/cascade/scripts/sign_plugin.sh`

Plugin Directories
- C++ plugins live in `${CASCADE_PLUGIN_DIR}`.
- Python plugins live in `${CASCADE_PYPLUGIN_DIR}`.
- Place `plugin_pubkey.pem` in each plugin directory to enable signature verification.
