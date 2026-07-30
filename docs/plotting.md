# Plotting

Cascade provides two plotting surfaces:

- C++ `PlotManager` for ROOT histograms and graphs;
- Python `plt_plot_manager` for NumPy arrays and Matplotlib.

Both are helpers rather than persistent object stores. The caller owns input
data and chooses where figures are saved.

## ROOT PlotManager

The ROOT API builds a `PlotSpec` from stack and overlay items:

```cpp
#include "PlotManager.hh"

#include <memory>

DrawSpec backgroundDraw;
backgroundDraw.SetNormBinWidth().SetLegendOpt("f");

DrawSpec dataDraw;
dataDraw.SetLegendOpt("pe").SetZeroError(false);

PlotSpec spec = PlotSpec::Simple()
                    .X("m_{bc} [GeV/c^{2}]")
                    .Y("Events / bin")
                    .UseRatio(true)
                    .Stack({
                        StackItemSpec(backgroundA, "Background A",
                                      ColorSpec(kBlue + 1, kBlue - 9, kBlue + 1),
                                      backgroundDraw),
                        StackItemSpec(backgroundB, "Background B",
                                      ColorSpec(kOrange + 7, kOrange - 2, kOrange + 7),
                                      backgroundDraw),
                    })
                    .Overlay({
                        OverlaySpec::Hist(data, "Data",
                                          ColorSpec(kBlack, 0, kBlack),
                                          dataDraw, true),
                    })
                    .RatioDenStack();

PlotManager plotter;
std::unique_ptr<TCanvas> canvas(plotter.Draw(spec, "mass_plot"));
const auto output = StageOutput("plots/mass.pdf");
canvas->SaveAs(output.string().c_str());
```

`Draw()` returns a newly allocated `TCanvas`; the caller must delete it. A
`std::unique_ptr<TCanvas>` is the simplest ownership policy.

## View transformations do not mutate inputs

`DrawSpec` can request rebinning, smoothing, scaling, bin-width normalization,
visibility, and legend behavior. `PlotManager` clones supplied histograms and
graphs before it applies rendering transformations. The original analysis
objects remain unchanged.

This makes one histogram safe to reuse in plots with different rebins or
normalizations.

## Stack and overlay roles

- `StackItemSpec` contributes a histogram to the filled stack.
- `OverlaySpec::Hist`, `Graph`, and `GraphAsymm` draw non-stacked objects.
- `IsData=true` marks a histogram as the default ratio numerator.
- `RatioRole::Numerator` explicitly marks a histogram ratio numerator.
- `Draw.Visible=false` removes an item from rendering, ratios, and legends.
- `Draw.VisibleInLegend=false` keeps a visible item out of the legend.

With `RatioDenStack()`, the denominator is the sum of visible stack histograms.
With `RatioDenOverlay(label)`, the named visible histogram overlay is used when
the label resolves uniquely. If neither method selects a denominator and there
is no visible stack, one visible histogram may use `RatioRole::Denominator`.

Invalid plot specifications fail before a canvas is created. This includes null
visible objects, 2D stack histograms, missing or ambiguous ratio roles, and
incompatible stack or ratio binning.

## Layout and style

`PlotSpec` contains:

| Field | Purpose |
| --- | --- |
| `Theme` | Fonts, margins, label sizes, ticks, logarithmic Y |
| `Layout` | Canvas size, ratio split, explicit Y range |
| `Legend` | Position, columns, automatic/manual mode |
| `Band` | Stack statistical uncertainty band |
| `Ratio` | Ratio range, unity line, MC uncertainty, denominator |
| `Sample` | Experiment label, comment, luminosity |
| `Cut` | Optional upper/lower cut arrows |

Fluent helpers cover common settings:

```cpp
spec.LogY()
    .LegendBox(0.58, 0.62, 0.88, 0.88, 1)
    .RatioDenStack();
```

For unusual styling, install hooks before `Draw()`:

```cpp
plotter.OnLegend([](TLegend &legend) {
    legend.SetTextSize(0.035);
});
plotter.OnMainFrame([](TH1 &frame) {
    frame.GetXaxis()->SetNdivisions(505);
});
plotter.OnPostRender([](TCanvas &canvas) {
    canvas.Modified();
});
```

Hooks also exist for pads, ratio frame, experiment text, and luminosity text.

## Transactional plot output

`PlotManager` does not know about module transactions. Within a module, call
`StageOutput` before `SaveAs`, as in the first example. Standalone plotting code
may save directly but does not receive rollback.

## Matplotlib manager

The Python helper accepts NumPy arrays:

```python
import numpy as np

from cascade import plt_plot_manager


plotter = plt_plot_manager(
    outdir=str(self.stage_output("plots")),
    ext="pdf",
    experiment="BelleII",
    lumi=427.9,
)

data = np.asarray(data_values)
mc = np.asarray(mc_values)
plotter.compare_data_mc(
    data,
    mc,
    bins=40,
    range=(5.2, 5.3),
    xlabel=r"$m_{bc}$ [GeV/$c^2$]",
    with_ratio=True,
)
plotter.save_all(prefix="mass")
```

Staging a directory is supported: all figures saved beneath it are promoted
together at commit. NumPy and Matplotlib are optional runtime dependencies.

Available helpers:

- `plot_hist` for one distribution;
- `plot_stack` for stacked arrays;
- `compare_data_mc` for data/MC, statistical bands, and an optional ratio;
- `set_experiment` for an available style preset;
- `save_all` for every figure accumulated by the manager.

For unweighted data, `compare_data_mc` uses symmetric square-root count errors.
For weighted data and MC, it uses the square root of `sumw2`. Empty MC bins are
excluded from finite ratio points.
