# Toy dimuon resonance analysis

This plugin package is a self-contained physics-style Cascade example. It
generates a deterministic toy `Z -> mu+ mu-` sample, selects events with ROOT
RDataFrame, fits the mass peak, and writes a portable Markdown/JSON report.
No external dataset or RooFit installation is required.

The generator is intentionally a teaching model, not a detector simulation or
precision Standard Model prediction.

## Pipeline

```text
ToyDimuonSourceModule (C++/ROOT)
  toy_dimuons.root + generation.json
                  |
                  v
DimuonSpectrumModule (C++/ROOT RDataFrame)
  dimuon_spectrum.root + cutflow.json
                  |
                  v
ResonanceFitModule (C++/ROOT)
  dimuon_fit.root + fit_result.json + dimuon_fit.png
                  |
                  v
MassReportModule (ROOT-free Python)
  analysis_summary.json + dimuon_report.md
```

| Module | Purpose | Main controls |
| --- | --- | --- |
| `ToyDimuonSourceModule` | Generate signal and continuum dimuon candidates | event count, seed, mass, width, signal fraction |
| `DimuonSpectrumModule` | Apply opposite-sign, kinematic, and mass selections | muon pT, eta, mass range and binning |
| `ResonanceFitModule` | Fit the selected mass spectrum and create a pull plot | fit range, mass seed, resolution seed, fixed/floating width |
| `MassReportModule` | Combine all JSON products into a human-readable report | input and report filenames |

The fit model is a Voigt profile (Breit-Wigner convolved with Gaussian detector
resolution) plus an exponential continuum. The natural width is fixed by
default so the compact toy sample gives a stable mass and resolution fit. Pass
`--float-width` to demonstrate a simultaneous width fit.

## Build and install

Install Cascade and make sure its environment is active, then run:

```bash
cd examples/plugins/toy_dimuon_analysis
cascade plugin install . --prefix ~/.local
cascade doctor plugins
```

The package's `cascade-plugin.yaml` declares its three ROOT-dependent C++
modules. No `SConstruct` is needed. The Python report remains ROOT-free and can
run in a worker without importing PyROOT.

## Run from Python

```bash
python3 run_analysis.py --output example-output
```

Run the same command again to exercise the snapshot cache. The second run
should report cached/skipped nodes while retaining the committed products.

Useful variants:

```bash
python3 run_analysis.py --events 100000 --output large-output
python3 run_analysis.py --float-width --output floating-width-output
python3 run_analysis.py --isolated --output isolated-output
python3 run_analysis.py --force --output example-output
```

`--isolated` moves each lifecycle into its configured worker process. `--force`
bypasses otherwise valid cache entries.

## Run declaratively

The equivalent workflow is checked in as `workflow.yaml`:

```bash
cascade dag validate workflow.yaml
cascade dag run workflow.yaml
```

Its default output directory is `analysis-output`.

## Inspect the result

The most useful products are:

- `dimuon_fit.png`: data, total fit, signal/background components, and pulls;
- `dimuon_report.md`: generated truth, cutflow, fitted values, and mass pull;
- `analysis_summary.json`: combined machine-readable result;
- `toy-dimuon-analysis.dot`: resolved DAG;
- workflow and per-module provenance under the output directory;
- `.cache/`: snapshot cache entries used on subsequent identical runs.

Try changing only `fit_min` or `float_natural_width` in `workflow.yaml`. Cascade
can reuse the generator and spectrum while invalidating the fit and downstream
report, which makes the DAG/cache boundary visible without a large dataset.
