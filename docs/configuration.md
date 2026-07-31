# Configuration schema

Cascade separates two kinds of configuration:

- `AnalysisManager` YAML documents describe ROOT inputs, cuts, and histograms;
- module parameter YAML/JSON documents assign values to parameters already
  registered by a module.

They are intentionally different formats.

## Analysis config version

Every input, cut, and histogram document requires:

```yaml
schema_version: 1
```

Missing or unsupported versions fail preflight. This prevents older files from
silently acquiring new semantics.

## Input config

```yaml
schema_version: 1

input:
  files:
    - data/run-001.root
    - data/run-002.root
  tree: events

branches:
  event:
    name: event_number
    type: Long64_t
  pt:
    name: jet_pt
    type: Float_t
  accepted:
    name: pass_selection
    type: Bool_t
```

`input.files` must be a non-empty sequence. Preflight opens every file and verifies
that the configured tree and scalar branches exist in all of them.

Supported classic branch types:

| ROOT spelling | C++ spelling | Value returned by `GetValue` |
| --- | --- | --- |
| `Double_t` | `double` | `double` |
| `Float_t` | `float` | `double` |
| `Int_t` | `int` | `double` |
| `UInt_t` | `unsigned int` | `double` |
| `Long64_t` | `long long` | `double` |
| `ULong64_t` | `unsigned long long` | `double` |
| `Bool_t` | `bool` | `0.0` or `1.0` |

The `type` field is optional. `BuildChain()` infers it from the scalar leaf when
omitted. An explicit type is useful because preflight then reports differences
before branch attachment.

Aliases such as `pt` are used in cuts and histogram expressions. The original ROOT
name remains in `name`.

```cpp
AnalysisManager manager;
auto report = manager.PreflightInputConfig("input.yaml");
if (!report.Valid())
{
    for (const auto &error : report.Errors)
        std::cerr << error << '\n';
}
manager.LoadInputConfig("input.yaml");
TChain *chain = manager.BuildChain();
```

`LoadInputConfig` runs the same preflight automatically and throws one aggregated
diagnostic.

## Cut config

```yaml
schema_version: 1

cuts:
  positive_pt: pt > 0
  selected: accepted && pt > 25
  central: abs(eta) < 2.5
```

Cut values are classic ROOT expressions. Aliases from the loaded input config are
expanded before `TTreeFormula` compilation.

Recommended order:

```cpp
manager.LoadInputConfig("input.yaml");
manager.BuildChain();
manager.PreflightCutConfig("cuts.yaml").ThrowIfInvalid("cuts.yaml");
manager.LoadCutConfig("cuts.yaml");
manager.EnableAllCuts();
```

Preflighting after `BuildChain()` enables formula compilation against the actual
tree. Structural validation still works before a tree is available.

Select individual cuts with:

```cpp
manager.EnableCuts({"positive_pt", "central"});
```

## Histogram config

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

Each `bins` value is:

```text
[positive integer number of bins, finite minimum, finite maximum]
```

The maximum must be greater than the minimum. Histogram expressions use the same
alias expansion as cuts.

```cpp
manager.PreflightHistogramConfig("histograms.yaml").ThrowIfInvalid("histograms.yaml");
manager.LoadHistogramConfig("histograms.yaml");
```

When the classic tree exists, preflight compiles every expression. RDF histogram
booking uses the same raw configuration structure.

## Generating config

Use framework writers when possible:

```cpp
AnalysisManager::WriteInputConfig(tree, "input.yaml", {"events.root"});
manager.WriteCutConfig("cuts.yaml");
manager.WriteHistogramConfig("histograms.yaml");
```

Generated files include the current schema version.

Runtime RDF lambda filters are intentionally not part of the YAML format.
`WriteCutConfig` rejects a manager containing one, and preflight rejects
`--lambda:` markers from external files because a callable cannot be restored
from text. Register those filters in module code after loading serializable
cuts.

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

Preflight checks structure and the resources it can inspect. It does not prove:

- remote files will remain available during execution;
- a formula has the intended physics meaning;
- every event value is finite;
- output storage has enough space;
- a module's custom external dependencies are valid.

Module-specific semantic checks still belong in `Init`.
