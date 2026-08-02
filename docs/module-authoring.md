# Writing analysis modules

This guide describes the contract shared by C++ `IAnalysisModule` and Python
`base_module`. Packaging, verification, and optional signing are covered
separately in [Plugin development and distribution](plugins.md).

## Lifecycle responsibilities

| Phase | Put this here | Avoid |
| --- | --- | --- |
| Constructor | Register parameters, set basename/code hash, static metadata | Opening inputs or creating final outputs |
| `Init` / `init` | Validate parameters, load config, build managers and inputs | Long event loops |
| `Check` | Framework-owned dry-run and snapshot-cache decision | User implementation; this phase is automatic |
| `Execute` / `execute` | Event loops, RDF definitions, transformations | Publishing final output paths directly |
| `Finalize` / `finalize` | Serialize staged trees, histograms, summaries, metadata | Irreversible external side effects |
| `Commit` | Framework-owned output and cache commit | User implementation; this phase is automatic |
| `OnFailure` / `on_failure` | Best-effort cleanup not covered by RAII/context managers | Throwing another exception |

All lifecycle exceptions are converted to `RunResult`. Destructors, RAII, and
Python context managers remain the preferred resource cleanup mechanism.

## C++ module

### Header

```cpp
#pragma once

#include "IAnalysisModule.hh"

class SelectionModule final : public IAnalysisModule
{
  public:
    SelectionModule();
    void Description() const override;
    ModuleMetadata GetMetadata() const override;

  protected:
    void Init() override;
    void Execute() override;
    void Finalize() override;
    void OnFailure(ModulePhase phase, const std::string &message) override;
};
```

### Implementation

```cpp
#include "SelectionModule.hh"

#include "Logger.hh"

SelectionModule::SelectionModule()
{
    m_Basename = "SelectionModule";
    m_CodeVersionHash = "replace-at-build-time";
    m_Param.Register<std::string>("input_config", "input.yaml");
    m_Param.Register<std::string>("cut_config", "cuts.yaml");
    m_Param.Register<std::string>("histogram_config", "histograms.yaml");
    m_Param.Register<std::string>("output", "histograms.root");
    m_Param.Register<double>("weight", 1.0);
}

void SelectionModule::Description() const
{
    LOG_INFO(m_Basename, "Runs a classic TTree selection.");
}

ModuleMetadata SelectionModule::GetMetadata() const
{
    ModuleMetadata metadata;
    metadata.Name = m_Basename;
    metadata.Version = "1.0.0";
    metadata.Summary = "Example event selection";
    metadata.Tags = {"selection", "classic-tree"};
    return metadata;
}

void SelectionModule::Init()
{
    auto *manager = Am();
    manager->LoadInputConfig(m_Param.Get<std::string>("input_config"));
    if (!manager->BuildChain()) throw std::runtime_error("cannot build input chain");
    manager->LoadCutConfig(m_Param.Get<std::string>("cut_config"));
    manager->EnableAllCuts();
    manager->LoadHistogramConfig(m_Param.Get<std::string>("histogram_config"));
}

void SelectionModule::Execute()
{
    auto *manager = Am();
    for (Long64_t index = 0; index < manager->GetEntryCount(); ++index)
    {
        if (IsCancellationRequested()) return;
        manager->LoadEvent(index);
        if (manager->PassesAllCuts()) manager->FillHistograms(m_Param.Get<double>("weight"));
    }
}

void SelectionModule::Finalize()
{
    Am()->WriteHistograms(StageOutput(m_Param.Get<std::string>("output")).string());
}

void SelectionModule::OnFailure(ModulePhase phase, const std::string &message)
{
    LOG_ERROR(m_Basename, "Failure in " << ToString(phase) << ": " << message);
}
```

The framework creates the `main` analysis manager before `Init`. Use `Am()` for it.
Call `RegisterAnalysisManager("name")` only when a module needs additional isolated
manager state.

## Python module

Python plugin files must end in `module.py`.

```python
import json

from cascade.pymodule.base_module import base_module


class SummaryModule(base_module):
    VERSION = "1.0.0"
    SUMMARY = "Builds a JSON summary."
    TAGS = ["summary", "python"]

    def __init__(self):
        super().__init__()
        self.basename = "SummaryModule"
        self.code_version_hash = "replace-at-build-time"
        self.summary = self.SUMMARY
        self.tags = list(self.TAGS)
        self.register_param("input", "events_manifest.json")
        self.register_param("output", "summary.json")
        self.register_param("label", "nominal")

    def print_description(self):
        print(self.SUMMARY)

    def init(self):
        input_path = self.final_output(self.get_param("input"))
        if not input_path.is_file():
            raise FileNotFoundError(input_path)
        self.track_input(input_path)

    def execute(self):
        with self.final_output(self.get_param("input")).open("r", encoding="utf-8") as source:
            payload = json.load(source)
        payload["label"] = self.get_param("label")
        with self.stage_output(self.get_param("output")).open("w", encoding="utf-8") as output:
            json.dump(payload, output, indent=2)
            output.write("\n")

    def finalize(self):
        pass

    def on_failure(self, phase, message):
        pass

    def snapshot_state(self):
        return {"format": 1}
```

`snapshot_state()` is the Python extension point for deterministic state that is
not already represented by registered parameters or the execution context.

## Typed parameters

Both languages pre-register parameters. External assignment of an unknown key is
rejected.

C++ supported types:

- `std::monostate`;
- `bool`, `int`, `long`, `long long`, `double`, `std::string`;
- `std::vector<int>`, `std::vector<double>`, `std::vector<std::string>`;
- `MixedVector`.

Python supports:

- `None`, `bool`, `int`, `float`, `str`;
- flat lists of scalar values.

Numeric coercion is deliberately narrow. For example, an integral `double` can
populate an integer parameter, but a fractional value cannot. For a C++ module
handle, serialize the current contract before hand-editing:

```python
module.save_params_to_yaml("params.yaml")
module.save_params_to_json("params.json")
```

Python modules load flat YAML with `set_param_from_yaml()` and expose resolved
values through `get_parameters()`.

See [Parameters](parameters.md) for coercion rules and complete serialized
formats.

The two common parameters are registered automatically:

| Parameter | Effect |
| --- | --- |
| `dry_run` | Return `Skipped` before execution; C++ modules also print manager/parameter summaries |
| `force_run` | Ignore an existing snapshot-cache match |

## Module metadata

Metadata can be listed without registering a runnable module instance:

```python
for item in controller.get_list_available_module_metadata():
    print(item["name"], item["language"], item["summary"])
```

For Python discovery, declare either one `METADATA` dictionary:

```python
class SummaryModule(base_module):
    METADATA = {
        "name": "SummaryModule",
        "version": "1.0.0",
        "summary": "Builds a JSON summary.",
        "tags": ["summary", "python"],
    }
```

or the `VERSION`, `SUMMARY`, and `TAGS` class attributes shown earlier. This
avoids constructing the class during normal metadata listing.

For a C++ instance, overriding `GetMetadata()` supplies runtime metadata. Plugin
discovery itself uses the registry's static metadata provider. The generated
`CASCADE_REGISTER_MODULE` entry supplies the class name and Cascade version.
Provide a custom entry when discovery should also expose summary, tags, or an
independent module version:

```cpp
#include "PluginABI.hh"
#include "SelectionModule.hh"

namespace
{
ModuleMetadata SelectionMetadata()
{
    ModuleMetadata metadata;
    metadata.Name = "SelectionModule";
    metadata.Version = "1.0.0";
    metadata.Summary = "Runs a classic TTree selection.";
    metadata.Tags = {"selection", "classic-tree"};
    return metadata;
}
} // namespace

CASCADE_PLUGIN_EXPORT_ABI

CASCADE_PLUGIN_EXPORT void CascadeRegisterPlugin()
{
    CASCADE_REGISTER_MODULE_WITH_METADATA(
        SelectionModule,
        SelectionMetadata());
}
```

Put this definition in the module source. The plugin build detects
`CascadeRegisterPlugin` and does not generate a second entry point.

## Transactional output

`StageOutput("relative/path")` and `stage_output("relative/path")` return a path
under the run-specific staging directory. The framework:

1. verifies every registered staged file exists;
2. journals the promotion plan;
3. backs up existing final files;
4. promotes all staged files;
5. writes the snapshot cache;
6. removes backups after success.

Failure at any point restores the previous output set. A module may stage multiple
files in one transaction.

Use `FinalOutput`/`final_output` to resolve an already committed input produced by
an upstream module. Both staging and final-path helpers reject paths outside the
configured output root.

Call `TrackInput(path)` / `track_input(path)` for every material file input.
Cascade hashes declared local inputs in the module provenance manifest. Output
artifacts registered with the staging helper are captured automatically.

Direct writes, network calls, database updates, and messages sent to external
services are not transactional.

## Cancellation

Long loops should cooperate:

```cpp
for (Long64_t index = 0; index < entries; ++index)
{
    if (IsCancellationRequested()) return;
    // Work
}
```

```python
for item in items:
    if self.is_cancellation_requested():
        return
    # Work
```

Returning after cancellation is enough. The framework observes the token before
commit and returns `Interrupted`, rolling outputs back.

## Snapshot design

The default snapshot includes:

- module basename and code hash;
- registered parameter values;
- every registered `AnalysisManager` snapshot;
- execution state that affects output identity.

Do not hide meaningful inputs in global variables. Put file names, systematic
choices, calibration versions, and thresholds in registered parameters or manager
configuration. Python modules can add stable custom fields via `snapshot_state()`.

`force_run` bypasses lookup but still records the completed snapshot.

## In-process versus isolated execution

Use in-process execution when later code needs fields mutated by the module object.
Use isolated execution for modules that load risky native libraries or process
untrusted/corrupt data:

```python
result = controller.run_module_isolated("selection")
```

Only durable effects cross the process boundary:

- committed files;
- snapshot-cache updates;
- the serialized `RunResult`.

Managers, member variables, Python objects, and other child memory do not return to
the parent.

## Author checklist

- [ ] Class name is globally unique across installed C++ and Python plugins.
- [ ] Basename and code-version hash are set.
- [ ] Every external parameter is registered with a stable type.
- [ ] `Init` rejects invalid parameter combinations.
- [ ] Long loops check cancellation.
- [ ] Protected outputs use staging helpers.
- [ ] Snapshot inputs are explicit and deterministic.
- [ ] Module metadata explains purpose and tags.
- [ ] Normal, cached, dry-run, failure, and isolated paths have been tested.
- [ ] Plugin package passes `cascade doctor plugins`.
