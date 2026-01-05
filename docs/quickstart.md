Quickstart

This is a single end-to-end example that exercises the main workflow:
- import the Python API
- register C++ and Python modules
- run a module directly

Example script: `examples/QuickstartExample.py`

Run:

```bash
python3 examples/QuickstartExample.py
```

What it does
- Lists available C++ modules
- Registers a C++ module instance
- Defines a lightweight Python module
- Runs the C++ module directly
- Prints module status and registered modules
- Saves a run log with `save_run_log_all()`
- Prints module metadata and progress

Notes
- This example avoids external configuration files.
- For a DAG run, see `examples/DAGMixedExample.py`.
- For ROOT macro usage, see `examples/RootMacroExample.C`.
