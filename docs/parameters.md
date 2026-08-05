# Parameters

Cascade parameters are a declared contract, not an open dictionary. A module
registers each key and its initial type in the constructor; later assignments
must target an existing key and remain compatible with that type.

## C++ registration and access

```cpp
Parameters().Register<std::string>(
    "input", "events.root", "Input ROOT file");
Parameters().Register<double>(
    "threshold", 0.5, "Selection threshold");
Parameters().Register<std::vector<std::string>>(
    "systematics", {"nominal"}, "Systematic labels");

const auto input = Parameters().Get<std::string>("input");
Parameters().Set<double>("threshold", 0.8);
```

`Has(key)` tests registration and `TypeOf(key)` returns the serialized type
name. `operator[]` is available, but explicit `Get<T>` and `Set<T>` make module
contracts easier to review.

## Supported C++ types

| C++ type | Serialized type |
| --- | --- |
| `std::monostate` | `none` |
| `bool` | `bool` |
| `int` | `int` |
| `long` | `long` |
| `long long` | `long long` |
| `double` | `double` |
| `std::string` | `string` |
| `std::vector<int>` | `vector<int>` |
| `std::vector<double>` | `vector<double>` |
| `std::vector<std::string>` | `vector<string>` |
| `MixedVector` | `vector<mixed>` |

`MixedVector` is a flat vector whose elements may be `long long`, `double`,
`std::string`, or `bool`. Nested sequences, maps, and null elements inside a
sequence are rejected.

## Assignment and coercion

Assignment preserves the registered target type:

- integer types accept an in-range integer or an integral floating-point value;
- `double` accepts integer and floating-point values;
- numeric vectors accept compatible numeric elements;
- string and boolean parameters require the same type;
- mixed vectors accept flat scalar vectors.

Fractional values cannot populate integer parameters. Overflow, unknown keys,
incompatible values, nested lists, and map/object values fail with an exception.

## Serialized YAML and JSON

The full format records value, type, and description:

```yaml
threshold:
  type: double
  value: 0.8
  description: Selection threshold
```

A compact value-only YAML document is also accepted:

```yaml
input: events.root
threshold: 0.8
systematics: [nominal, tracking_up]
force_run: false
```

The full format rejects a serialized `type` that disagrees with the registered
type. Loading never creates new parameters.

C++ `ParamManager` I/O:

```cpp
Parameters().LoadYAMLFile("params.yaml");
Parameters().SaveYAMLFile("resolved-params.yaml");
Parameters().LoadJSONFile("params.json");
Parameters().SaveJSONFile("resolved-params.json");
```

Python handles for C++ modules expose the same contract:

```python
module.load_param_from_yaml("params.yaml")
module.save_params_to_yaml("resolved-params.yaml")
module.save_params_to_json("resolved-params.json")
```

## Python modules

Python modules use native scalar values and flat scalar lists:

```python
self.register_param("input", "events.root")
self.register_param("threshold", 0.5)
self.register_param("labels", ["nominal"])

self.set_param("threshold", 0.8)
threshold = self.get_param("threshold")
resolved = self.get_parameters()
```

Load a flat YAML mapping with:

```python
self.set_param_from_yaml("params.yaml")
```

As in C++, registration fixes the expected value category and unknown keys are
rejected.

## Framework parameters

Every module receives two parameters automatically:

| Key | Default | Meaning |
| --- | --- | --- |
| `dry_run` | `false` | Skip analysis execution; C++ also prints registered summaries |
| `force_run` | `false` | Bypass a matching snapshot-cache lookup |

Do not register these names again.

## Parameters and snapshots

All registered parameter values participate in the default snapshot identity.
Prefer parameters for every output-affecting user choice:

- input paths and dataset versions;
- selections and systematic labels;
- calibration identifiers;
- output format choices;
- thresholds and algorithm settings.

This makes cache hits explainable and parameter files sufficient to reproduce a
run.

## DAG parameter links

The controller copies registered values immediately before a target node runs:

```python
controller.link_dag_parameter(
    "calibration", "output_tag",
    "selection", "calibration_tag",
)
```

This works for C++/C++, Python/Python, and mixed-language module pairs. Both keys
must exist and the target's registered type still controls coercion. The source
node must be a dependency of the target.

The generic `DAGManager` remains independent of `ParamManager`; controller-level
links are implemented using a named data-transfer callback. See
[DAG execution](dag.md).
