Troubleshooting

Plugin Load Fails
- Error: "Plugin public key not found; skipping plugin load"
  - Fix: place `plugin_pubkey.pem` in `${CASCADE_PLUGIN_DIR}`.
- Error: "Plugin signature invalid"
  - Fix: re-sign the plugin with `scripts/sign_plugin.sh`.
- Error: "Plugin ABI mismatch"
  - Fix: rebuild the plugin against the current Cascade headers and re-sign it.

Python Plugins Not Listed
- Ensure files end with `module.py`.
- Ensure `plugin_pubkey.pem` exists in `${CASCADE_PYPLUGIN_DIR}` and `module.py.sig` is present.

_cascade Import Errors
- Verify `_cascade.so` symlink exists in `${PYTHONDIR}` and points to `libCascade.so`.
- Reinstall if the symlink is missing.

Runtime Crash After Plugin Update
- Ensure plugins are built with `-DCASCADE_PLUGIN_NO_AUTO_REGISTER`.
- Remove old `.so` files before reinstalling new ones.
