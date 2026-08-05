# AnalysisManager

`AnalysisManager` is the ROOT-facing state owned by an analysis module. It
supports two execution styles:

- a classic `TTree` event loop;
- a lazy ROOT `RDataFrame` graph.

Use one style per manager. In particular, classic cut enabling and RDF filters
represent different execution models and should not be mixed on the same
manager.

## Configuration and preflight

The three configuration loaders validate before mutating manager state:

```cpp
manager->LoadInputConfig("input.yaml");
manager->LoadCutConfig("cuts.yaml");
manager->LoadHistogramConfig("histograms.yaml");
```

For tooling that needs to collect errors without loading anything, use:

```cpp
const auto result = manager->PreflightInputConfig("input.yaml");
if (!result.Valid())
{
    for (const auto &error : result.Errors)
        std::cerr << error << '\n';
}
```

`PreflightCutConfig` and `PreflightHistogramConfig` follow the same contract.
See [Configuration schema](configuration.md) for schema version 1.

## Classic TTree loop

A complete classic loop has a fixed setup order:

```cpp
void SelectionModule::Init()
{
    auto *manager = Am();
    manager->LoadInputConfig(Parameters().Get<std::string>("input_config"));
    if (!manager->BuildChain())
        throw std::runtime_error("cannot build input chain");

    manager->LoadCutConfig(Parameters().Get<std::string>("cut_config"));
    manager->EnableAllCuts();
    manager->LoadHistogramConfig(Parameters().Get<std::string>("histogram_config"));
}

void SelectionModule::Execute()
{
    auto *manager = Am();
    const auto entries = manager->GetEntryCount();

    for (Long64_t index = 0; index < entries; ++index)
    {
        if (IsCancellationRequested()) return;
        manager->LoadEvent(index);
        if (manager->PassesAllCuts())
            manager->FillHistograms(1.0);
    }
}

void SelectionModule::Finalize()
{
    const auto output = StageOutput("histograms.root");
    Am()->WriteHistograms(output.string());
}
```

`LoadEvent` updates progress periodically. `GetValue(alias)` reads a configured
branch or a registered derived variable after the current event has been loaded.

To select only part of a cut file, replace `EnableAllCuts()` with:

```cpp
manager->EnableCuts({"good_track", "signal_region"});
```

The enabled set is used by `PassesAllCuts()`. `PassesCut(name)` and
`PassesCuts(names)` are available for explicit branching.

## Trees, branches, and derived values

`BuildChain()` creates and owns the input `TChain` described by the input
configuration. For a tree created elsewhere:

```cpp
manager->RegisterTree(tree, ResourceOwnership::Borrowed);
```

The default is borrowed ownership. Use `ResourceOwnership::Owned` only when the
manager should delete the object.

Classic derived variables are stored as `double` values:

```cpp
double *mass2 = manager->RegisterVariable("mass2");
*mass2 = computed_mass2;
```

`AttachBranch` connects configured aliases to a tree for reading or writing.
`WriteTrees(path)` serializes the registered trees. Pass a staged path when it is
called from a module:

```cpp
manager->WriteTrees(StageOutput("selected.root").string());
```

## RDataFrame pipeline

Initialize RDF either from schema-versioned input configuration:

```cpp
manager->InitRdfFromConfig("input.yaml");
```

or directly from a tree and ROOT file:

```cpp
manager->InitRdfFromFile("events", "events.root");
```

Then define columns, load cuts, apply filters, and book lazy actions:

```cpp
manager->DefineRdfVariable("pt2", "pt * pt");
manager->LoadCutConfig("cuts.yaml");
manager->ApplyAllRdfFilters();

const auto validation =
    manager->PreflightHistogramConfig("histograms.yaml");
validation.ThrowIfInvalid("histograms.yaml");
manager->BookRdfHistogramsFromConfig("histograms.yaml", "nominal");
```

Loading a cut file replaces the manager's current cut set. Once an RDF filter
has been applied, that set is immutable: reload or replacement is rejected so
the recorded cut state cannot diverge from the already-built RDF graph.
Callable RDF filters are runtime-only and cannot be written to or restored from
cut YAML.

`BookRdfHistogramsFromConfig` consumes the histogram section but does not call
preflight itself. Call `PreflightHistogramConfig` explicitly when the file came
from outside the application. When a histogram expression names a column created
only with `DefineRdfVariable`, classic preflight cannot see that RDF-only column;
the RDF booking step is then the authoritative expression check.

Writing all booked histograms executes the lazy graph once:

```cpp
manager->WriteRdfHistograms(
    StageOutput("rdf-histograms.root").string());
```

The implementation submits the progress counter and all booked histograms to
`ROOT::RDF::RunGraphs`, so the actions share the event loop.

## RDF snapshots

```cpp
manager->WriteRdfSnapshot(
    "selected",
    StageOutput("selected.root").string(),
    TreeOpt::Recreate);
```

The current column selection contract is:

| Option | Snapshot columns |
| --- | --- |
| `TreeOpt::Recreate` | Columns defined on the current RDF node |
| `TreeOpt::Append` | All columns visible on the current RDF node |

These options choose the column set; the actual ROOT snapshot is created with
ROOT's lazy snapshot action and executed together with the progress action.
Despite its name, `TreeOpt::Append` does not select ROOT file update mode in the
current implementation; it selects all visible columns.

## Forking RDF state

`Fork()` creates an independent manager view over the current RDF node:

```cpp
auto signal = manager->Fork();
auto control = manager->Fork();

signal->ApplyRdfFilter("signal", "score > 0.9");
control->ApplyRdfFilter("control", "score < 0.2");
```

The forks keep the shared input chain alive while their filter and histogram
graphs remain independent. Destroying the parent manager does not invalidate a
fork.

## Histogram and tree ownership

Externally registered ROOT objects are borrowed by default:

```cpp
manager->RegisterHistogram(
    "mass", histogram, "nominal", ResourceOwnership::Borrowed);
```

Choose `Owned` only if no other owner will delete the object. The same rule
applies to `RegisterTree`.

## Output boundary

`WriteHistograms`, `WriteTrees`, `WriteRdfHistograms`, `WriteRdfSnapshot`, and
`OpenOutputFile` write exactly the path supplied by the caller. They do not
automatically register that path with the module output transaction.

Inside a module, resolve every protected output first:

```cpp
const auto path = StageOutput("reports/counts.txt");
auto output = Am()->OpenOutputFile(path.string());
```

Outside a module, direct paths are allowed but do not receive rollback or atomic
promotion.

## State, metadata, and progress

- `SnapshotState()` returns deterministic manager state used by module caching.
- `WriteMetadata()` writes analysis provenance alongside an output file.
- `GetProgress()` reports a value from zero to one for supported event loops.
- `PrintConfigSummary`, `PrintCutSummary`, and `PrintHistogramSummary` are useful
  for diagnostics and C++ dry runs.

The module lifecycle automatically includes every registered manager snapshot in
the cache identity. Do not place output-affecting state in an unregistered global.
