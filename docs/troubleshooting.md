# Troubleshooting

Start by separating the failure layer:

1. dependency discovery/build;
2. runtime library/Python import;
3. plugin verification/loading;
4. module registration/configuration;
5. lifecycle execution/commit.

## Build failures

### `root-config: command not found`

Initialize the intended ROOT installation and verify:

```bash
command -v root-config
root-config --version
root-config --cflags --libs
```

Do not build Cascade against one ROOT installation and plugins against another.

### yaml-cpp discovery fails

```bash
pkg-config --modversion yaml-cpp
pkg-config --cflags --libs yaml-cpp
```

Fix the `PKG_CONFIG_PATH` or install the development package.

### pybind11 include probe fails

```bash
python3 -m pybind11 --includes
```

Install pybind11 for the same Python interpreter used by SCons.

### Link succeeds but tests cannot find libraries

Run tests through:

```bash
scons test -j2
```

The target configures build-tree library paths. For manual execution, include the
installed library directory in `LD_LIBRARY_PATH`.

## Python import failures

If a newly installed Cascade reports missing C++ symbols while an older Cascade is
also installed, inspect `LD_LIBRARY_PATH` first. ELF `RUNPATH` deliberately has
lower priority than `LD_LIBRARY_PATH`, so an old `libAMCM.so` or manager library in
that variable can be selected ahead of the libraries beside the active
`libCascade.so`. Remove the stale entry or start from the intended Cascade runtime
environment, then verify with `ldd`.

### `No module named cascade`

For the default layout, `PYTHONPATH` contains the parent of the `cascade`
directory:

```bash
export PYTHONPATH="${CASCADE_PREFIX}/lib:${PYTHONPATH}"
```

Inspect:

```bash
python3 -c 'import cascade; print(cascade.__file__)'
```

### `_cascade.so` cannot load

Verify the symlink and dependencies:

```bash
ls -l "${CASCADE_PREFIX}/lib/cascade/_cascade.so"
ldd "${CASCADE_PREFIX}/lib/cascade/_cascade.so"
```

Ensure `${CASCADE_PREFIX}/lib` and the correct ROOT library directory are visible
to the dynamic loader.

### Wrong installation is imported

```bash
python3 -c 'import cascade; print(cascade.__file__, cascade.__version__, cascade.__abi_tag__)'
```

Remove unintended prefixes from `PYTHONPATH` rather than mixing releases.

## Plugin failures

Always start with:

```bash
cascade doctor plugins
```

### Plugin disappears in a new terminal

Environment-only plugin roots are temporary. Register the installation prefix:

```bash
cascade plugin path add /path/to/plugin-prefix
cascade plugin path list
```

The prefix is the directory containing `lib/cascade/plugin` and
`lib/cascade/pyplugin`, not either package root itself. Prefer
`cascade plugin install SOURCE --prefix PREFIX` to install and register in one
transaction.

### Trust store missing or empty

This affects only commands using `--require-signed`. Install the publisher public
key in:

```text
${CASCADE_PLUGIN_TRUST_STORE}
```

Keys inside a plugin package do not grant trust.

### Plugin manifest missing

`scons` only builds. Install the package to generate its verified manifest:

```bash
scons install
```

Use `cascade plugin install --private-key ... --public-key ...` for a signed
distribution. Low-level plugin SCons builds are intentionally unsigned.

### Signature invalid

The manifest changed, the wrong public key is installed, or the signature belongs
to another manifest. Regenerate and sign after the final build.

### Hash mismatch

An installed module changed after manifest generation. Reinstall the package; do
not edit the hash manually.

### ABI mismatch

The integer plugin ABI differs. Rebuild against the current Cascade headers.

### ABI tag mismatch

ABI 1 matches but compiler, standard library, ROOT, pointer width, or build mode
differs. Compare:

```python
import cascade
print(cascade.__abi_tag__)
```

Rebuild Cascade and the plugin with one toolchain.

### Python module is absent

Check:

- file name ends in `module.py`;
- class directly inherits `base_module`;
- class is listed in the verified manifest;
- file hash matches;
- class name does not collide with a C++ or Python module.

### Duplicate module registration

Module class names are global across active plugin roots. Rename one class/package
entry. Controller instance names must also be unique within one controller.

## Configuration failures

### `schema_version is required`

Add to input, cut, and histogram documents:

```yaml
schema_version: 1
```

Do not add this field to parameter YAML.

### Input preflight reports many errors

This is intentional aggregation. Fix the first structural errors before resource
errors:

1. root is a map;
2. schema is supported;
3. `input.files` and `input.tree` exist;
4. `branches` is a map;
5. files open;
6. trees and scalar branches exist;
7. types match.

### Cut/histogram expression is invalid

Load and build the input chain before expression preflight so aliases can be
expanded and `TTreeFormula` can compile against the actual tree.

### Parameter key is not registered

Register it in the module constructor/`__init__`. Parameter loading never invents
new public parameters.

### Parameter type mismatch

Inspect the resolved contract:

```python
print(module.get_parameters())
```

For C++ handles, `dump_params_to_yaml()` includes registered types.

## Lifecycle and output failures

Inspect all result fields:

```python
result = controller.run_module("instance")
print(result.status, result.phase, result.message)
```

### Module is unexpectedly `Skipped`

- `dry_run=true` skips at `Check`;
- a matching snapshot skips when `force_run=false`.

Set `force_run=true` only when rerunning the same snapshot is intentional.

### Output file never appears

Check that:

- the module called `stage_output`/`StageOutput`;
- the staged file was actually created;
- `RunResult` is `Done`;
- the configured output root is the expected directory.

Staged files are deliberately invisible at final paths before commit.

### Existing output disappeared after failure

This should not occur for registered staged output. Check whether the module wrote
directly to the final path. Direct writes are outside rollback.

### Commit fails

Typical causes:

- cache root is not a directory or is not writable;
- output parent is not writable;
- staged file was registered but never created;
- cross-module output collision;
- filesystem rename/remove failure.

The result phase is `Commit`; previously existing registered outputs should be
restored.

### Snapshot cache appears stale

The snapshot only knows explicit parameters, manager state, code hash, and
execution state. Make hidden external inputs explicit. For one diagnostic rerun,
set `force_run=true`.

Cache defaults:

```text
C++/Python: ~/.cache/cascade/snapshot_cache/<module>.yaml
```

Each schema-versioned cache entry links its hash to the successful module
provenance manifest. A cache-hit manifest records that source path, which makes it
possible to distinguish a stale cache decision from the run that originally
created the snapshot.

Inspect the single schema-versioned cache format through `cascade cache list`.
Python and C++ modules use the same core cache manager and locking rules.

## Isolated execution failures

### `terminated by signal N`

A fatal native signal was contained. Inspect plugin/native library logs and rerun
under a debugger in-process only in a safe development environment.

### `exited without a valid result`

The child exited before completing the result protocol. Causes include `_exit`,
fatal runtime shutdown, or corruption before result serialization.

### Parent module fields did not change

Expected: isolated child memory is not copied back. Read committed output instead.

### Isolation hangs

Request cancellation from another control thread. Cascade sends `SIGTERM` and then
`SIGKILL` if needed. Also inspect external I/O calls that do not respond to signals.

## DAG failures

### Missing dependency or cycle

DAG validation runs before execution. Every dependency name must identify a node,
and cycles are rejected.

### Parameter link fails

Controller DAG parameter links require:

- both module instances registered with the controller;
- source and target keys already registered;
- the source node to be a dependency of the target;
- target type compatible with the source value.

The same API supports C++, Python, and mixed-language pairs.

### DAG result contains pending nodes

With `fail_fast=True`, unrelated nodes remain `Pending` after the first failure.
Run with `fail_fast=False` to finish independent branches:

```python
result = controller.run_dag(fail_fast=False)
```

After correcting a failed branch, call `dag.reset_failed()` before retrying.
Controller nodes added through `add_module_to_dag` already convert module failure
results into DAG failures; low-level callback nodes signal failure by throwing.

## Information to collect

For a reproducible bug report, include:

```bash
root-config --version
python3 --version
c++ --version
python3 -c 'import cascade; print(cascade.__version__, cascade.__abi_version__, cascade.__abi_tag__)'
cascade doctor plugins
```

Also include the terminal `RunResult`, relevant config with secrets removed, and
whether execution was in-process or isolated.
