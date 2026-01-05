Examples

Quickstart (End-to-End)
- Example script: `examples/QuickstartExample.py`
- Runs a C++ module and a Python module, then executes a DAG.

Run:

```bash
python3 examples/QuickstartExample.py
```

DAG with Mixed C++ and Python Modules
- Example script: `examples/DAGMixedExample.py`
- C++ module: `ExampleModule` (registered by name)
- Python module: `py_stage` (defined in the script)

Run:

```bash
python3 examples/DAGMixedExample.py
```

The script emits `dag_example.dot` to visualize the DAG.

What it does
- Registers a built-in C++ module using `ctrl.register_module("HistModule", "cpp_stage")`.
- Registers a Python module instance with `ctrl.register_python_module("py_stage", py_stage())`.
- Adds two DAG nodes:
  - `cpp_stage` runs first.
  - `py_stage` depends on `cpp_stage`.
- Executes the DAG with `ctrl.run_dag()`.

Customize
- Replace `HistModule` with any C++ module name from `ctrl.get_list_available_modules()`.
- Add parameters through the module APIs before `run_dag()`.
- Use `dag.dump_dot("name.dot")` and render with Graphviz:

```bash
dot -Tpng dag_example.dot -o dag_example.png
```

ROOT Macro Example
- Example macro: `examples/RootMacroExample.C`
- Shows how to parse the JSON parameter file passed by the CLI wrapper.

Run (via CLI wrapper):

```bash
./python/cascade --macro examples/RootMacroExample.C --set n=1000 --set mode="fast"
```

Plugin Example
- Example plugin: `examples/PluginExample.cc`
- Demonstrates ABI exports and manual registration.

Notes:
- Build the plugin with `-DCASCADE_PLUGIN_NO_AUTO_REGISTER`.
- Sign the resulting `libExamplePluginModule.so` and place it in `${CASCADE_PLUGIN_DIR}`.
