Quickstart

This is a single end-to-end example that exercises the main workflow:
- import the Python API
- list and run plugin modules
- run a module directly

Example script: `examples/QuickstartExample.py`

Run:

```bash
python3 examples/QuickstartExample.py
```

What it does
- Lists available plugin modules
- Registers an `ExampleModule` instance when the plugin package is installed
- Runs the C++ plugin module directly when available
- Prints module status and registered modules
- Prints module progress

Notes
- This example avoids external configuration files.
- Build, sign, and install `/home/hlee/cascade_plugin` before running this example.
- A typical signed install looks like:

```bash
cd /home/hlee/cascade_plugin
CASCADE_PLUGIN_PRIVATE_KEY=/path/to/plugin_private.pem \
CASCADE_PLUGIN_PUBLIC_KEY=/path/to/plugin_pubkey.pem \
scons install
```

- For a DAG run, see `examples/DAGMixedExample.py`.
- For ROOT macro usage, see `examples/RootMacroExample.C`.
