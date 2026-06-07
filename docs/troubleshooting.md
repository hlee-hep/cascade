Troubleshooting

Plugin Load Fails
- Start with:

```bash
cascade doctor plugins
```

- Error: "Plugin public key not found; skipping plugin load"
  - Fix: place `plugin_pubkey.pem` in `${CASCADE_PLUGIN_DIR}`.
- Error: "Signed plugin manifest missing"
  - Fix: generate and sign `plugin_manifest.json`.
- Error: "Plugin manifest signature invalid"
  - Fix: re-sign `plugin_manifest.json` with `scripts/sign_plugin.sh`.
- Error: "Plugin hash mismatch"
  - Fix: regenerate the manifest after rebuilding or reinstalling plugin files.
- Error: "Plugin ABI mismatch"
  - Fix: rebuild the plugin against the current Cascade headers and regenerate/sign the manifest.

Python Plugins Not Listed
- Ensure files end with `module.py`.
- Ensure `plugin_pubkey.pem`, `plugin_manifest.json`, and `plugin_manifest.json.sig` exist in `${CASCADE_PYPLUGIN_DIR}`.
- Run `cascade doctor plugins` and check class names reported for Python manifest entries.

Duplicate Module Names
- Error: "Duplicate module name"
  - Fix: rename one plugin module so every installed C++ and Python module has a globally unique name.

_cascade Import Errors
- Verify `_cascade.so` symlink exists in `${PYTHONDIR}` and points to `libCascade.so`.
- Reinstall if the symlink is missing.

Runtime Crash After Plugin Update
- Ensure plugins are built with `-DCASCADE_PLUGIN_NO_AUTO_REGISTER`.
- Remove old `.so` files before reinstalling new ones.

Snapshot Cache Location
- C++ module cache: `~/.cache/cascade/snapshot_cache/`
- Python module cache: `~/.cache/cascade/py_module_hashes.json`
