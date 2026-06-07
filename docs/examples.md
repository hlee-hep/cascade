Examples

Quickstart (End-to-End)
- Example script: `examples/QuickstartExample.py`
- Runs the example plugin C++ module when it is installed.

Run:

```bash
python3 examples/QuickstartExample.py
```

DAG with Plugin Module
- Example script: `examples/DAGMixedExample.py`
- C++ plugin module: `ExampleModule` from `/home/hlee/cascade_plugin` (registered by name)

Run:

```bash
python3 examples/DAGMixedExample.py
```

The script emits `dag_example.dot` to visualize the DAG.

What it does
- Registers a plugin C++ module using `ctrl.register_module("ExampleModule", "cpp_stage")`.
- Adds a DAG node for `cpp_stage`.
- Executes the DAG with `ctrl.run_dag()`.

Customize
- Replace `ExampleModule` with any plugin module name from `ctrl.get_list_available_modules()`.
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

Plugin Package Example
- Example plugin package: `/home/hlee/cascade_plugin`
- Demonstrates the supported manifest-based plugin build and install flow.

Notes:
- The package SConstruct follows the installed template at `${PREFIX}/share/cascade/scripts/plugin_sconstruct`.
- The template builds modules from `include/*.hh` and `src/*.cc`.
- It generates `plugin_manifest.json` for installed C++ and Python plugin files.
- Set `CASCADE_PLUGIN_PRIVATE_KEY` to sign manifests during install.
- Set `CASCADE_PLUGIN_PUBLIC_KEY` to install `plugin_pubkey.pem` beside the manifests.
- The quickstart and DAG examples expect `ExampleModule` from this package to be installed and loadable.

```bash
cd /home/hlee/cascade_plugin
CASCADE_PLUGIN_PRIVATE_KEY=/path/to/plugin_private.pem \
CASCADE_PLUGIN_PUBLIC_KEY=/path/to/plugin_pubkey.pem \
scons install
```

ROOT Managers Example
- Example macro: `examples/RootManagersExample.C`
- Demonstrates `AnalysisManager`, `ParamManager`, and `PlotManager` directly in ROOT.
