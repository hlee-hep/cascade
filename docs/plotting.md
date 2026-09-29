# Plotting

Cascade provides two plotting surfaces:

- C++ `PlotManager` for ROOT histograms and graphs;
- Python `PublicationFigure` for prepared ROOT or NumPy data and Matplotlib.

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

## NumPy input

`histogram_panel` prepares raw event arrays for the same `PublicationFigure`
renderer used by ROOT exports. NumPy input does not require PyROOT.

```python
from cascade import PublicationFigure, histogram_panel

panel = histogram_panel(
    data_values,
    backgrounds=[mc_values],
    labels=["MC"],
    bins=40,
    hist_range=(5.2, 5.3),
    ratio=True,
    xlabel=r"$m_{bc}$ [GeV/$c^2$]",
    sample={"enabled": True, "experiment": "Belle II",
            "comment": "Preliminary", "luminosity": 427.9},
)
figure = PublicationFigure(use_tex=False).add(panel)
figure.save(self.stage_output("plots/mass.pdf"))
```

Use only `data` with `data_draw="HIST"` for one step histogram, only
`backgrounds` for a stack, or both for a data/MC comparison. `weights` and
`background_weights` supply event weights. `density=True` normalizes data and
the combined background separately to unit area, including bin widths.
All inputs share bin edges, inferred from the combined event range when omitted.

The adapter uses symmetric square-root count errors for unweighted events and
`sqrt(sumw2)` for weighted events. Ratio points show numerator uncertainty;
the separate MC band shows denominator uncertainty. Nonpositive MC bins have
no ratio points or band. ROOT exports retain their prepared error model.
The renderer does not recalculate these quantities.

See [the NumPy example](../examples/publication_numpy.py) for a standalone run.

The old `plt_plot_manager` is removed. Replace `plot_hist`, `plot_stack`, and
`compare_data_mc` with the input combinations above, and `save_all` with explicit
`PublicationFigure.save` calls. Use `PublicationLayout` for multiple panels and
`PublicationStyle` for typography; no global Matplotlib style is changed.

## ROOT publication output

`PlotManager::SavePublication` adds an optional publication backend beside
`Draw`. It exports independent numerical snapshots from the same C++ render
plan, then runs the Matplotlib renderer with an argument vector (no shell).
The caller does not manage intermediate JSON files or restyle a ROOT canvas.

```cpp
PlotManager plotter;
spec.Sample.Comment = "Preliminary";
spec.Sample.Lumi = 428;
spec.Y("Candidates / bin");
plotter.SavePublication({spec}, "mass.pdf", "single");
plotter.SavePublication({electron, muon}, "comparison.pdf", "vertical2");
plotter.SavePublication({eMass, eEnergy, muMass, muEnergy}, "selection.pdf", "row4");
```

The optional fourth argument selects the Python executable (default `python3`).
Activate the intended Cascade environment first; that interpreter must be able
to import `cascade.publication`, NumPy, and Matplotlib. REVTeX typography also
requires LaTeX, dvipng, amsmath, type1cm, and type1ec. Missing dependencies cause
an actionable error instead of silently changing fonts. Module callers should
pass a `StageOutput(...)` path; standalone writes are replaced atomically only
after a successful render. Failure leaves an existing output file unchanged.

| Layout | Panels | Final size | Legend |
| --- | --- | --- | --- |
| `single` | 1 | 3.4 × 2.65 in | Inside the axes |
| `vertical2` | 2, top to bottom | 3.4 × 4.4 in | Inside each panel |
| `row4` | 4, left to right | 7 × 2.15 in | Shared above the panels |

Typography uses Computer Modern, 9 pt legends/ticks and 10 pt axis titles
(9 pt individual vertical-panel y titles). Belle II is bold and upright;
Preliminary/Simulation is regular. Luminosity is rounded to a whole number for
display only; Simulation omits it. Header edges sit inside the outer plot edges,
and the rendered header group is 3 pt above the frame. `row4` has no bin-width
annotation. Matching y titles are shared automatically; specify meaningful
`YTitle` values, particularly for unequal bin widths or normalized histograms.

Python can consume the same ROOT `PlotSpec` directly:

```python
from cascade import PublicationFigure

plot = PublicationFigure("row4", shared_ylabel="Candidates / bin")
for spec in (e_mass, e_energy, mu_mass, mu_energy):
    plot.add(spec)
plot.save("selection.pdf")
```

`add` owns a snapshot, so the source ROOT objects may be released afterwards.
It also accepts the dict or JSON string from `PlotManager.ExportPublication`.
Optional `xlabel`, `ylabel`, and `labels={old: new}` arguments let Python callers
supply publication notation explicitly. Common ROOT TLatex commands and square
bracket axis units are translated; explicit `$...$` labels pass through. For
example, use the Python raw string `r"$q_{\mathrm{BDT}}$"`.
Unrecognized ROOT commands fail with a request for an explicit LaTeX label.
`use_tex=False` is an explicit mathtext fallback, not the REVTeX font guarantee.
`render()` returns a Matplotlib figure; `save()` also scopes export settings and
uses atomic file replacement. PDF, PNG, and SVG are supported.

Numerical behavior follows ROOT `Draw`: cloned histograms receive view
transformations, data use ROOT Poisson errors, `ZeroError=false` suppresses
zero-count data points, negative overlay scales normalize to the visible stack,
and the MC band uses PlotManager's effective-count gamma intervals. Prepared
`TGraph`/`TGraphAsymmErrors` points and errors are copied without fitting or
interpolation. Colors, visibility, legend filtering, and ordering come from the
specification; ROOT font/marker sizing and canvas hooks do not override the
publication preset. Cut thresholds are shown as vertical dashed lines.

The backend supports 1D histograms/stacks, ratio panels, graph overlays, and
TH2 color maps. Supported 1D draw options are HIST, L, P, PE, PE0, E, E0, E1,
LP/PL, and graph bands 2/3. TH2 accepts COL/COLZ (empty means COLZ). Unsupported
options are rejected rather than omitted.
Shared headers require identical Sample metadata across all panels.

Complete examples: `examples/PublicationPlot.C` and
`examples/publication_plot.py`. The ROOT API stays available without any of the
optional publication dependencies; importing `cascade` does not import
Matplotlib or NumPy.


### Independent layout and style

The preset names above are compatibility shortcuts, not the layout model.
`PublicationFigure` renders numerical snapshots; `PublicationLayout` controls
the grid and physical canvas size; `PublicationStyle` controls typography and
marks.
Experiment name, comment, and luminosity come from `PlotSpec.Sample`; none is a
layout constant. Computer Modern and an upright bold experiment header remain
the default publication convention.

```python
from cascade import PublicationFigure, PublicationLayout, PublicationStyle

layout = PublicationLayout(rows=2, columns=3, width=7, height=4.6,
                           left=.08, right=.025, bottom=.12, top=.16,
                           hgap=.055, vgap=.16, legend="shared", share_x=True)
style = PublicationStyle(font_size=10, axis_size=11, legend_size=10,
                         marker_size=4)
figure = PublicationFigure(layout, style=style)
for spec in specifications:
    figure.add(spec)
figure.save("selection.pdf")
```

Width/height are inches; margins and gaps are fractions of the full canvas.
Panels fill the grid left-to-right, top-to-bottom; trailing cells may be empty.
Choose enough height and top margin for the requested font size and legend.
Shared legends occupy a separate row below the sample header. Set
`legend_columns` to wrap them; `legend="inside"` uses `legend_location`, and
`legend="none"` suppresses them. These choices never change numerical data.
`share_x` omits repeated x titles above another panel when all x titles match;
`share_y` uses a common y title when all y titles match. Neither option forces
common numerical limits. Disable `panel_labels` to omit panel letters.

C++ exposes the same geometry and style through `PublicationOptions`:

```cpp
PublicationOptions options;
options.Layout.Rows = 2;
options.Layout.Columns = 2;
options.Layout.Width = 7;
options.Layout.Height = 5;
options.Layout.Legend = "shared";
options.Layout.LegendColumns = 3;
options.Layout.ShareX = true;
options.Style.FontSize = 10;
options.Style.AxisSize = 11;
plotter.SavePublication({a, b, c}, "grid.pdf", options);
```

The optional fourth argument still selects the Python executable. Layout and
style values travel in the request document independently of the panel data.
Invalid grid dimensions, margins, options, or sizes fail without replacing an
existing output. Existing string-preset calls retain their original panel
counts and geometry. `single`, `vertical2`, and `row4` have no figure-number
semantics. The narrow `row4` preset retains its approved inline shared legend;
custom layouts use a separate shared-legend row.


### Ratio panels and TH2 maps

A grid cell can contain a main plot with a ratio beneath it. Turn on the
existing `PlotSpec.Ratio`; numerator/denominator selection follows ROOT Draw,
including stack denominators, explicit overlay labels, and ratio roles.

```cpp
spec.UseRatio(true).RatioDenStack();
spec.Ratio.YLabel = "Data / MC";
spec.Ratio.YMin = 0.2;
spec.Ratio.YMax = 1.8;
spec.Layout.RatioSplit = 0.30;
PublicationOptions options;
options.Layout.Height = 3.8;
options.Layout.RatioGap = 4; // Physical points between the axes.
manager.SavePublication({spec}, "ratio.pdf", options);
```

Ratio points and their errors are computed in C++, after the same cloned view
transformations as Draw. The default `Band.Asymm=true` uses ROOT's Poisson
ratio intervals; negative bin contents are rejected for this mode.
`Band.Asymm=false` uses ROOT histogram division and symmetric propagated errors,
including signed weights. This also fixes the old ROOT Draw symmetric path,
which previously displayed numerator counts without division. The MC-error
band uses the denominator's effective-count intervals, divided by its absolute
bin content. Zero-denominator bins have neither a ratio point nor a band.
`Ratio.MCError`, `UnityLine`, `Arrow`, Y limits, and label are honored. Arrows
mark central values outside either displayed boundary. X limits are shared
with the main axis; the main x title/tick labels move to the ratio axis.
Give the canvas enough height for both axes at the selected font size.

For 2D, put one TH2 into the overlays, optionally followed by graph overlays:

```cpp
TH2D density("density", "", 40, 1.4, 2.0, 40, -0.8, 0.6);
// Fill density using the analysis inputs.
density.GetZaxis()->SetTitle("Weighted events");
density.SetMinimum(0.05);
density.SetMaximum(100.0);
PlotSpec plane;
plane.X("M [GeV/c^{2}]").Y("#Delta E [GeV]");
plane.Sample.Comment = "Simulation";
plane.Theme.LogZ = true;
plane.Overlay({OverlaySpec::Hist(&density, "Density", ColorSpec(), DrawSpec().SetOpt("COLZ"))});
options.Style.ColorMap = "viridis";
options.Layout.ColorbarSpace = 0.22; // Fraction of the cell width, including labels.
manager.SavePublication({plane}, "density.pdf", options);
```

`Theme.LogZ` also enables the ROOT pad's logarithmic color scale. It is
independent of `Theme.LogY`, which controls the spatial y axis. Histogram
stored minimum/maximum define color limits; the histogram Z title labels the
colorbar. With no stored limits, the renderer derives limits from the bins.
Nonpositive bins are masked on a logarithmic color scale; an all-nonpositive
map is rejected. Linear color scales retain signed bins. COL omits the colorbar.
TH2 x/y bin edges, selected axis ranges, bin values, and errors are exported
without resampling, including variable-width bins and bin-area normalization.
Heatmaps are rasterized inside PDF/SVG while axes, text, and graph boundaries
remain vector objects. A TH2 panel accepts graph lines, points, or error bands
for regions/data; 1D stacks, multiple TH2 maps in one cell, TH3, and a ratio
beneath a TH2 are rejected. Contours/surface ROOT draw options are not mapped;
pass explicitly sampled boundaries as TGraph overlays.

Python uses the same `PublicationFigure.add(spec)` API. Set `ratio_gap` and
`colorbar_space` on `PublicationLayout`, and `color_map` on `PublicationStyle`.
A grid may mix ordinary 1D, ratio, and TH2 panels. Existing 1D preset geometry
is unchanged. Common sample-header metadata must still match across cells.
A complete runnable synthetic example, including a mixed grid and both native
and Python save calls, is `examples/publication_ratio_2d.py`:

```bash
python examples/publication_ratio_2d.py --output publication_examples
```
