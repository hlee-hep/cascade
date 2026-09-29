# Configuration schema

Analysis configuration is independent of ROOT execution and module parameters.
`AnalysisConfig.hh` contains plain cut and histogram specs in namespace `Cascade`.
Every cut/histogram YAML document requires `schema_version: 1`.

## Cuts

```yaml
schema_version: 1
cuts:
  positive_pt: pt > 0
  selected: accepted && pt > 25
  central: abs(eta) < 2.5
```

Names must be unique and non-empty; expressions must be non-empty scalar text.
Lambda markers cannot be restored from YAML and are rejected. Expressions are
passed unchanged to the chosen backend. Alias definitions belong in native ROOT
code. YAML order is preserved.

```cpp
auto cuts = Cascade::LoadCuts("cuts.yaml");
auto selected = Cascade::SelectCuts(cuts, {"positive_pt", "central"});
Cascade::WriteCuts("selected-cuts.yaml", selected);
```

An empty selection selects no cuts. Unknown or repeated selected names fail.

## Histograms

```yaml
schema_version: 1
histograms:
  jet_pt:
    expr: pt
    bins: [50, 0, 250]
  doubled_pt:
    expr: pt * 2
    bins: [100, 0, 500]
```

`bins` is `[positive integer number of bins, finite minimum, finite maximum]`.
The maximum must exceed the minimum and the count must fit a C++ `int`.
This convenience schema describes uniform 1D histograms. It does not restrict
native ROOT histogram models, types, or dimensionality used by module code.

```cpp
auto histograms = Cascade::LoadHistograms("histograms.yaml");
Cascade::WriteHistograms("copy.yaml", histograms);
```

## Validation

Loaders validate before returning specs and report the path and errors. Structural
preflight collects errors without opening ROOT files or compiling expressions:

```cpp
auto report = Cascade::PreflightCutConfig("cuts.yaml");
report.ThrowIfInvalid("cuts.yaml");
auto histReport = Cascade::PreflightHistogramConfig("histograms.yaml");
```

Malformed YAML, unsupported/missing schema versions, duplicate names, malformed
expressions, and invalid bins fail. ROOT checks actual columns/types/expressions
when native APIs or the optional helpers consume the specs. Writers also validate
before opening output files. See [Native ROOT](analysis-config.md) for examples.

Input files, trees, aliases, and branch types are configured directly through ROOT;
the former AnalysisManager input schema is no longer interpreted by Cascade.

## Parameter YAML

Module parameters do not use `schema_version`. A compact hand-written file can
assign plain values:

```yaml
force_run: false
dry_run: false
input_config: input.yaml
weight: 1.0
selected_systematics: [nominal, scale_up, scale_down]
```

Framework-generated YAML preserves type and description:

```yaml
weight:
  type: double
  value: 1.0
  description: Event weight
```

Both forms update parameters that the module already registered. Unknown keys,
incompatible values, and serialized type mismatches fail.

C++:

```cpp
module->LoadParamsFromYAML("params.yaml");
module->SaveParamsToJSON("resolved-params.json");
```

Python:

```python
import json

module.set_param_from_yaml("params.yaml")
with open("resolved-params.json", "w", encoding="utf-8") as output:
    json.dump(module.get_parameters(), output, indent=2)
```

## Paths and reproducibility

- Input file paths are interpreted by ROOT from the process working directory
  unless an absolute path or ROOT-supported URI is used.
- Module outputs should be relative to the configured output directory.
- Keep calibration versions, external dataset identifiers, and selection choices
  in registered parameters so they contribute to the snapshot.
- Do not place secrets in parameter files. Provenance applies key-based redaction,
  but an external credential provider is safer.

## Preflight boundary

Preflight checks YAML structure only. It does not prove:

- remote files will remain available during execution;
- a formula has the intended physics meaning;
- every event value is finite;
- output storage has enough space;
- a module's custom external dependencies are valid.

Module-specific semantic checks still belong in `Init`.
