# Analysis configuration and native ROOT

Cascade runs modules and manages caching, provenance, and transactional outputs.
Analysis code owns its ROOT objects, graph, event loop, and output actions.
`AnalysisConfig.hh` is a ROOT-independent YAML interface in namespace `Cascade`:

```cpp
auto cuts = Cascade::LoadCuts("cuts.yaml");
auto histograms = Cascade::LoadHistograms("histograms.yaml");
auto selectedCuts = Cascade::SelectCuts(cuts, {"quality", "signal"});
```

Loaders return ordinary `CutSpec` and `HistogramSpec` vectors in YAML document
order. `SelectCuts` follows the requested order, rejects unknown/duplicate names,
and selects nothing for an empty list. Pass the complete vector to use all cuts.
See [Configuration schema](configuration.md) for YAML syntax and validation.

## Optional RDF helpers

`RootAnalysisHelpers.hh` provides stateless adapters. They never run an event loop,
write an output, retain a current node, or alter ROOT's multithreading settings.

```cpp
#include "RootAnalysisHelpers.hh"
#include <ROOT/RDFHelpers.hxx>
#include <TFile.h>

ROOT::RDataFrame df("events", "events.root");
auto node = Cascade::ApplyCuts(df, selectedCuts);
auto weighted = node.Define("weight", "2.0");
auto histogram = Cascade::BookHistogram(weighted, histograms.at(0), "weight");
auto count = node.Count();

ROOT::RDF::RSnapshotOptions options;
options.fLazy = true;
auto snapshot = node.Snapshot("selected", StageOutput("selected.root").string(),
                              {"pt", "eta"}, options);
ROOT::RDF::RunGraphs({histogram, count, snapshot});
TFile output(StageOutput("histograms.root").c_str(), "RECREATE");
histogram->Write();
output.Close();
```

`ApplyCuts` returns a native `ROOT::RDF::RNode`. Branch from any node and call native
`Define`, `Filter`, `Histo2D`, `Vary`, `Snapshot`, or other ROOT APIs directly.
`BookHistogram` returns `RResultPtr<TH1D>`; its optional weight argument is a native
column name and its final optional argument overrides the output histogram name.
An expression already naming a column is used directly; other expressions get a
local derived column on the booking branch. The caller retains the returned
results and controls execution. Use `HistogramModel(spec)` with native ROOT
booking APIs if only conversion of uniform binning is needed. More elaborate
binning and histogram types can use native models directly.

## Native TTree

Use the same specs with native ROOT APIs. No branch binding, scalar conversion,
object ownership, or event loop is imposed by Cascade:

```cpp
const auto cut = cuts.at(0);
const auto spec = histograms.at(0);
TTreeFormula selection(cut.Name.c_str(), cut.Expression.c_str(), &tree);
TTreeFormula value("value", spec.Expression.c_str(), &tree);
TH1D histogram(spec.Name.c_str(), "", spec.Bins, spec.Min, spec.Max);
histogram.SetDirectory(nullptr);
for (Long64_t index = 0; index < tree.GetEntries(); ++index)
{
    if (IsCancellationRequested()) return;
    tree.GetEntry(index);
    if (selection.EvalInstance()) histogram.Fill(value.EvalInstance());
}
```

This is a scalar formula example for one TTree. For TChains, array selections,
vector branches, or object branches, use ROOT's corresponding reader/formula
semantics directly, including formula leaf updates on chain transitions.
Expressions are backend-specific: structural YAML validation does not compile
RDF expressions or TTree formulas.

## Module responsibilities

In `Init`, call `TrackInput(path)` for each local data/configuration file and load
configuration there, before the cache check. Register selected cut names, tree
names, weights, remote dataset versions, and other output-affecting choices as
parameters. For non-parameter state override `AnalysisSnapshotState()` and return
a deterministic string. Native graphs and captured lambda state are not inspected.
Python modules keep `track_input()` and `snapshot_state()`.

Use `StageOutput()` for every protected output. ROOT owns its usual object
lifetimes; close files before returning from the phase that writes them. Existing
module provenance records tracked inputs and staged artifacts without a manager.
Report optional named progress with `ReportProgress(fraction, "events")` from C++.
Progress is cleared before each run and accepts finite fractions in [0, 1].

`UsesRoot()` defaults to true and keeps in-process ROOT modules on the existing
serialized execution lane. ROOT-free C++ modules may override it to false.
This scheduling declaration does not change ROOT's own implicit MT policy.

## Migration from AnalysisManager

This is a breaking C++ API change; rebuild plugins against plugin ABI 4.
`AnalysisManager`, `LambdaManager`, `Am`, manager registration, `TreeOpt`, and
manager-owned input/branch/output APIs have been removed. Replace
`UsesRoot()` overrides with `UsesRoot()`.

Cut and uniform 1D histogram YAML retain schema version 1. Use `LoadCuts`,
`LoadHistograms`, their `Preflight*Config` functions, and `WriteCuts`/`WriteHistograms`.
Filters now follow document or explicit selection order, not sorted map order.
Histogram names default to the YAML key; pass an explicit name when preserving an
old `hist_<alias>_<prefix>` output name. Bind old input aliases explicitly with
native ROOT APIs; input YAML/automatic scalar binding are no longer framework APIs.
Create files, chains, snapshots, and metadata directly using ROOT. ROOT-file
metadata formerly written by the manager must be migrated explicitly if needed;
Cascade's external provenance manifest remains available.

Snapshot schema 5 separates new cache identities from manager-era snapshots.
Use a fresh installation prefix when migrating, so obsolete manager libraries and
ROOT dictionaries from an older installation are not left on the library path.
