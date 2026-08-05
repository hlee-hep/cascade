# DAG execution

`DAGManager` is a bounded dependency scheduler. It validates the complete graph
before running, records every node state, propagates failures to dependent nodes,
and returns a `DAGRunResult`.

Controller-managed modules are assigned execution lanes automatically:

- in-process modules using `AnalysisManager` enter the process-wide ROOT lane;
- in-process Python modules share the ROOT-safe serial lane because of the GIL and unknown global state;
- C++ modules that override `UsesAnalysisManagers()` to return `false` may run in parallel;
- isolated modules may run concurrently in separate worker processes.

Only one in-process ROOT module runs at a time, including across controller
instances. Set `CASCADE_DAG_MAX_WORKERS` to a positive integer to bound concurrent
ROOT-free and isolated work. The default is the detected hardware concurrency.

## Module DAGs from Python

Use the controller-level API for mixed C++/Python workflows:

```python
from cascade import py_amcm


controller = py_amcm()
controller.register_module("TextProducerModule", "producer")
controller.register_module("TextTransformModule", "transform")

controller.add_module_to_dag("producer")
controller.add_module_to_dag("transform", ["producer"])

result = controller.run_dag()
if result.failed():
    for node in result.nodes:
        if not node.succeeded():
            print(node.name, node.status, node.message)
```

`add_module_to_dag` converts a module `Failed` or `Interrupted` result into a
failed DAG node automatically. `Done` and cache/dry-run `Skipped` results allow
dependents to proceed.

Set `isolated=True` per node when required:

```python
controller.add_module_to_dag(
    "native_reader",
    isolated=True,
)
```

## Node states

| State | Meaning |
| --- | --- |
| `Pending` | Not attempted |
| `Running` | Callback is active |
| `Succeeded` | Callback completed without an exception |
| `Failed` | Callback or incoming data link threw |
| `Blocked` | A dependency failed or was blocked |

`DAGRunResult.succeeded()` is true only when every node succeeded.
`DAGRunResult.failed()` is true when at least one node failed or was blocked.

## Validate before execution

For declarative workflows, validate plugin discovery, parameters, links, and
graph structure without running modules:

```bash
cascade dag validate workflow.yaml
cascade dag validate workflow.yaml --json
```

Validation constructs registered module instances, so constructors should remain
limited to parameter registration and inexpensive metadata setup. It does not
call `Init`, `Check`, `Execute`, `Finalize`, or `Commit`, and does not create the
configured output files.

Structural errors such as cycles, missing dependencies, or invalid data links
throw before node execution. Task failures are captured in node results.

## Failure policy

The default is fail-fast:

```python
result = controller.run_dag(fail_fast=True)
```

When a node fails, all of its pending descendants become `Blocked`; unrelated
nodes that have not started remain `Pending`. Work already dispatched is allowed
to finish. The scheduler is completion-driven: when one node finishes, newly ready
dependents can start immediately without waiting for unrelated nodes from the same
ready set.

To finish independent branches:

```python
result = controller.run_dag(fail_fast=False)
```

In this mode a failed branch is blocked while nodes that do not depend on it
continue.

## Retry and reset

Node state persists after execution:

```python
dag = controller.get_dag()

dag.reset_failed()
retry = controller.run_dag()
```

`reset_failed()` changes only `Failed` and `Blocked` nodes back to `Pending`;
previously successful nodes are preserved. `reset()` marks every node pending and
reruns the entire graph.

Calling `run_dag()` without either reset does not repeat completed or failed
nodes.

## Parameter links

Controller-level parameter links work across C++ and Python modules:

```python
controller.link_dag_parameter(
    "calibration",
    "output_tag",
    "selection",
    "calibration_tag",
)
```

Both parameters must already be registered. The source node must be a direct or
transitive dependency of the target. Immediately before the target runs, the
source value is copied and checked against the target's registered type.

The source is a parameter value, not an automatically discovered output file.
For large data or isolated execution, prefer committed files:

1. the producer stages and commits a file;
2. the consumer depends on the producer;
3. the consumer opens the path through `FinalOutput` or `final_output`.

## C++ controller API

For registered C++ modules:

```cpp
AMCM controller;
controller.RegisterModule("PrepareModule", "prepare");
controller.RegisterModule("SelectionModule", "select");

controller.AddModuleToDAG("prepare", {});
controller.AddModuleToDAG("select", {"prepare"});
controller.LinkDAGModuleParameter(
    "prepare", "dataset",
    "select", "input_dataset");

const DAGRunResult result = controller.RunDAG();
if (result.Failed())
{
    for (const auto &node : result.Nodes)
        if (!node.Succeeded())
            std::cerr << node.Name << ": " << node.Message << '\n';
}
```

`AddModuleToDAG(..., true)` selects isolated execution for that node.

## Generic callback DAG

Use `DAGManager` directly when a node is not a module:

```cpp
DAGManager dag;

dag.AddNode("download", {}, [] {
    DownloadDataset();
}, DAGExecutionLane::Parallel);
dag.AddNode("index", {"download"}, [] {
    BuildIndex();
}, DAGExecutionLane::Parallel);

const auto result = dag.Execute(false);
```

A callback signals failure by throwing. The exception message is stored in the
node result. Generic callbacks default to `DAGExecutionLane::Serial`, preserving
exclusive deterministic execution unless a lane is selected explicitly.

## Generic data links

`DAGManager` is no longer coupled to `ParamManager`. A low-level data link is a
named transfer callback:

```cpp
dag.AddDataLink(
    "download",
    "index",
    "manifest",
    [&] {
        indexConfig = downloadManifest;
    });
```

The callback runs after all dependencies have succeeded and immediately before
the target task. Transfer exceptions fail the target with a message that names
the link.

## Graph mutation

Node addition, data-link addition, reset, and nested execution are rejected while
the DAG is executing. Node names and dependency entries must be non-empty,
dependencies cannot be duplicated, and a node cannot depend on itself.

The graph itself may be extended between runs. Existing node states remain until
one of the reset operations is called.

## DOT output

```python
dag.dump_dot("pipeline.dot")
```

Dependency edges are solid and data links are dotted and labeled. Nodes include
their current state and use state-specific colors. Names and labels are escaped
before being written.

Render with Graphviz:

```bash
dot -Tpng pipeline.dot -o pipeline.png
```

## Operational limits

- Scheduling is local and bounded; it is not a distributed executor.
- There is no automatic backoff or retry count. Isolated-module timeouts remain
  controlled by `CASCADE_ISOLATED_TIMEOUT_SECONDS`.
- Ready-node selection is deterministic, but completion order for parallel lanes
  is intentionally not deterministic.
- Output path collisions remain a module-design error.
- External side effects are not covered by the module output transaction.
