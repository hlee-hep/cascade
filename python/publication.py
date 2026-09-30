"""Publication output for prepared ROOT or NumPy numerical data.

Optional dependencies are loaded only when publication data are used.
No histogram rebinning, normalization, fitting, or error estimation occurs here.
"""
from copy import deepcopy
from dataclasses import dataclass
import importlib.util
import math
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


@dataclass(frozen=True)
class PublicationLayout:
    """Physical canvas size in inches; margins and gaps are canvas fractions.

    Panels fill the grid in row-major order. Unused trailing cells stay empty.
    Shared axis titles require identical labels; numerical axis limits stay local.
    """
    rows: int = 1
    columns: int = 1
    width: float = 3.4
    height: float = 2.65
    left: float = .17
    right: float = .06
    bottom: float = .20
    top: float = .13
    hgap: float = .08
    vgap: float = .12
    legend: str = "inside"
    legend_location: str = "upper right"
    legend_columns: int = 0
    share_x: bool = False
    share_y: bool = True
    panel_labels: bool = True
    ratio_gap: float = 4
    colorbar_space: float = .22
    # Compatibility geometry belongs to presets, never renderer branches.
    compact_ticks: bool = False
    ylabel_size: float = 0
    wrap_ylabel: bool = False
    shared_ylabel_x: float = .025
    inline_legend: bool = False

    def __post_init__(self):
        for name in ("rows", "columns"):
            value = getattr(self, name)
            if type(value) is not int or value < 1:
                raise ValueError(f"{name} must be a positive integer")
        if type(self.legend_columns) is not int or self.legend_columns < 0:
            raise ValueError("legend_columns must be a nonnegative integer")
        for name in ("width", "height"):
            if not math.isfinite(getattr(self, name)) or getattr(self, name) <= 0:
                raise ValueError(f"{name} must be finite and positive")
        for name in ("left", "right", "bottom", "top", "hgap", "vgap", "ylabel_size", "shared_ylabel_x", "ratio_gap", "colorbar_space"):
            if not math.isfinite(getattr(self, name)) or getattr(self, name) < 0:
                raise ValueError(f"{name} must be finite and nonnegative")
        if not 0 < self.colorbar_space < .8:
            raise ValueError("colorbar_space must be between zero and 0.8")
        if self.legend not in ("inside", "shared", "none"):
            raise ValueError("legend must be inside, shared, or none")
        if self.legend_location not in ("upper right", "upper left", "lower right", "lower left", "best"):
            raise ValueError("Unsupported inside legend location")
        if 1-self.left-self.right-(self.columns-1)*self.hgap <= 0 or 1-self.top-self.bottom-(self.rows-1)*self.vgap <= 0:
            raise ValueError("Margins and gaps leave no panel area")

    def rectangles(self, count):
        if not 1 <= count <= self.rows*self.columns:
            raise ValueError("Panel count must fit the requested grid")
        width = (1-self.left-self.right-(self.columns-1)*self.hgap)/self.columns
        height = (1-self.top-self.bottom-(self.rows-1)*self.vgap)/self.rows
        return [(self.left+(i % self.columns)*(width+self.hgap),
                 1-self.top-(i//self.columns+1)*height-(i//self.columns)*self.vgap,
                 width, height) for i in range(count)]


@dataclass(frozen=True)
class PublicationStyle:
    """Typography and marks, independent of panel arrangement and sample data."""
    font_size: float = 9
    axis_size: float = 10
    legend_size: float = 9
    marker_size: float = 3.2
    line_width: float = 1.5
    header_gap: float = 3
    use_tex: bool = True
    color_map: str = "viridis"
    dpi: float = 220

    def __post_init__(self):
        for name in ("font_size", "axis_size", "legend_size", "marker_size", "line_width", "dpi"):
            if not math.isfinite(getattr(self, name)) or getattr(self, name) <= 0:
                raise ValueError(f"{name} must be finite and positive")
        if not math.isfinite(self.header_gap) or self.header_gap < 0:
            raise ValueError("header_gap must be finite and nonnegative")


_LAYOUTS = {
    "single": PublicationLayout(),
    "vertical2": PublicationLayout(rows=2, height=4.4, left=.14, right=.09,
        bottom=.12, top=.085, vgap=.095, share_x=True, ylabel_size=9, wrap_ylabel=True),
    "row4": PublicationLayout(columns=4, width=7, height=2.15, left=.055,
        right=.018, bottom=.20, top=.16, hgap=.029, legend="shared",
        compact_ticks=True, shared_ylabel_x=.012, inline_legend=True),
}


def _text(value):
    """Translate common ROOT TLatex labels; explicit $...$ labels pass through."""
    value = str(value).strip()
    if "$" in value:
        return value
    def units(unit):
        return re.sub(r"\b(GeV|MeV|TeV|keV|eV|cm|mm|fb|pb|c)\b", r"\\mathrm{\1}", unit.strip())
    match = re.fullmatch(r"(.*?)\s*\[\s*(.*?)\s*\]", value)
    if match:
        return _text(match[1]) + " ($" + units(match[2]) + "$)"
    match = re.fullmatch(r"(Candidates|Events)(.*?)(GeV.*|MeV.*)", value)
    if match:
        return match[1] + match[2] + "$" + units(match[3]) + "$"
    if value in ("M", "E", "q", "X", "Y", "N"):
        return "$" + value + "$"
    if value == "#DeltaE":
        value = "#Delta E"
    if "#" in value or "_" in value or "^" in value:
        allowed = {"Delta", "delta", "tau", "mu", "eta", "pi", "gamma", "theta", "phi", "sigma",
                   "alpha", "beta", "rho", "ell", "bar", "overline", "sqrt", "frac", "mathrm", "it", "bf"}
        for command in re.findall(r"#([A-Za-z]+)", value):
            if command not in allowed:
                raise ValueError(f"Unsupported ROOT label command #{command}; provide an explicit LaTeX $...$ label")
        value = re.sub(r"#([A-Za-z]+)", lambda m: "\\" + {"it": "mathit", "bf": "mathbf"}.get(m[1], m[1]), value)
        return "$" + units(value) + "$"
    return value.replace("%", r"\%").replace("&", r"\&")


def _values(item, key, length=None, nonnegative=False):
    import numpy as np
    values = np.asarray(item[key], dtype=float)
    if values.ndim != 1 or not np.isfinite(values).all():
        raise ValueError(f"Publication {key} must be a finite one-dimensional array")
    if length is not None and len(values) != length:
        raise ValueError(f"Publication {key} has inconsistent length")
    if nonnegative and (values < 0).any():
        raise ValueError(f"Publication {key} must be nonnegative")
    return values


def _graph_values(item):
    y = _values(item, "y")
    _values(item, "x", len(y))
    for key in ("xerror_low", "xerror_high", "error_low", "error_high"):
        _values(item, key, len(y), True)


def _heatmap_values(item):
    import numpy as np
    x, y = _values(item, "xedges"), _values(item, "yedges")
    if len(x) < 2 or len(y) < 2 or not (np.diff(x) > 0).all() or not (np.diff(y) > 0).all():
        raise ValueError("TH2 bin edges must be strictly increasing")
    for key in ("values", "errors"):
        values = np.asarray(item[key], dtype=float)
        if values.shape != (len(y)-1, len(x)-1) or not np.isfinite(values).all():
            raise ValueError(f"TH2 {key} must be a finite [y][x] array matching its edges")
        if key == "errors" and (values < 0).any():
            raise ValueError("TH2 errors must be nonnegative")
    for key in ("zmin", "zmax"):
        value = item[key]
        if value is not None and (not math.isfinite(value) or (item["log_z"] and value <= 0)):
            raise ValueError("Color limits must be finite and positive for a logarithmic scale")
    if item["zmin"] is not None and item["zmax"] is not None and item["zmin"] >= item["zmax"]:
        raise ValueError("Color maximum must exceed color minimum")
    if item["log_z"] and not (np.asarray(item["values"]) > 0).any():
        raise ValueError("Logarithmic TH2 color scale needs at least one positive bin")
    ylim = _values(item, "ylim", 2)
    if ylim[0] >= ylim[1]:
        raise ValueError("TH2 Y range must be increasing")


def _validate(panel):
    if panel.get("schema_version") != 1:
        raise ValueError("Unsupported PlotManager publication schema; expected version 1")
    if not panel.get("stacks") and not panel.get("overlays"):
        raise ValueError("Publication panel has no visible items")
    if any(item["kind"] != "hist" for item in panel["stacks"]):
        raise ValueError("Publication stacks require 1D histograms")
    heatmaps = [item for item in panel["overlays"] if item["kind"] == "hist2d"]
    if heatmaps and (len(heatmaps) != 1 or panel["stacks"] or "ratio" in panel or
                     any(item["kind"] == "hist" for item in panel["overlays"])):
        raise ValueError("A 2D panel accepts one TH2 plus graph overlays, without stacks or ratio")
    if "ratio" in panel:
        ratio = panel["ratio"]
        _graph_values(ratio["points"])
        if "band" in ratio:
            _graph_values(ratio["band"])
        if not math.isfinite(ratio["split"]) or not 0 < ratio["split"] < 1:
            raise ValueError("Ratio split must be between zero and one")
        if not math.isfinite(ratio["ymin"]) or not math.isfinite(ratio["ymax"]) or ratio["ymin"] >= ratio["ymax"]:
            raise ValueError("Ratio Y limits must be finite and increasing")
    for item in panel["stacks"] + panel["overlays"] + ([panel["band"]] if "band" in panel else []):
        if item["kind"] == "hist2d":
            _heatmap_values(item)
            option = item.get("draw", "").upper().replace("SAME", "").strip()
            if option not in ("", "COL", "COLZ"):
                raise ValueError("Publication TH2 draw options are COL and COLZ; use graph overlays for region boundaries")
            continue
        if item["kind"] == "hist":
            values = _values(item, "values")
            edges = _values(item, "edges", len(values) + 1)
            if not (edges[1:] > edges[:-1]).all():
                raise ValueError("Publication histogram edges must be strictly increasing")
        elif item["kind"] == "graph":
            values = _values(item, "y")
            _values(item, "x", len(values))
            for key in ("xerror_low", "xerror_high"):
                _values(item, key, len(values), True)
        else:
            raise ValueError("Publication supports prepared 1D/2D histograms and graphs")
        for key in ("error_low", "error_high"):
            _values(item, key, len(values), True)
        option = item.get("draw", "").upper().replace("SAME", "").strip()
        if item["kind"] == "hist" and option in ("L", "LP", "PL", "2", "3"):
            raise ValueError(f"Histogram draw option {option!r} is not supported; use HIST or error bars")
        if item["kind"] == "graph" and option == "HIST":
            raise ValueError("Graph draw option HIST is not supported; use L or PE")
        if option not in ("", "HIST", "L", "P", "PE", "PE0", "E", "E0", "E1", "LP", "PL", "2", "3"):
            raise ValueError(f"Unsupported publication draw option {option!r}; use HIST, L, PE, or 3")


class PublicationFigure:
    """Accumulate independent prepared panels and save with a paper layout.

    Layout geometry and publication style are independent. ROOT canvas hooks do
    not run. Object colors and numerical transformations come from the input adapter.
    A figure owns copies of JSON data, not the caller's ROOT objects.
    """
    def __init__(self, layout="single", *, style=None, use_tex=None, shared_ylabel=None):
        self.preset = layout if isinstance(layout, str) else None
        if isinstance(layout, str):
            if layout not in _LAYOUTS:
                raise ValueError(f"Unknown publication preset {layout!r}")
            layout = _LAYOUTS[layout]
        if not isinstance(layout, PublicationLayout):
            raise TypeError("layout must be a PublicationLayout or a preset name")
        self.layout = layout
        self.style = style or PublicationStyle()
        if not isinstance(self.style, PublicationStyle):
            raise TypeError("style must be a PublicationStyle")
        self.use_tex = self.style.use_tex if use_tex is None else bool(use_tex)
        self.shared_ylabel = shared_ylabel
        self.panels = []

    def add(self, spec, *, manager=None, xlabel=None, ylabel=None, labels=None):
        """Export a ROOT PlotSpec, or consume an already exported dict/JSON string."""
        if isinstance(spec, str):
            panel = json.loads(spec)
        elif isinstance(spec, dict):
            panel = deepcopy(spec)
        else:
            if manager is None:
                import ROOT
                if not hasattr(ROOT, "PlotManager"):
                    if ROOT.gSystem.Load("libPlotManager") < 0:
                        raise RuntimeError("Cannot load libPlotManager; activate the Cascade runtime first")
                manager = ROOT.PlotManager()
            panel = json.loads(str(manager.ExportPublication(spec)))
        if xlabel is not None:
            panel["xlabel"] = xlabel
        if ylabel is not None:
            panel["ylabel"] = ylabel
        if labels:
            for item in panel["stacks"] + panel["overlays"] + ([panel["band"]] if "band" in panel else []):
                item["label"] = labels.get(item["label"], item["label"])
        _validate(panel)
        self.panels.append(panel)
        return self

    def check_dependencies(self):
        """Report rendering readiness without importing plotting modules or writing files."""
        missing = []
        python = {name: importlib.util.find_spec(name) is not None for name in ("numpy", "matplotlib")}
        missing.extend(name for name, available in python.items() if not available)
        executables, packages = {}, {}
        if self.use_tex:
            executables = {name: shutil.which(name) for name in ("latex", "dvipng", "kpsewhich")}
            missing.extend(name for name, path in executables.items() if path is None)
            for package in ("amsmath.sty", "type1cm.sty", "type1ec.sty"):
                available = False
                if executables["kpsewhich"]:
                    try:
                        probe = subprocess.run([executables["kpsewhich"], package], capture_output=True,
                                               text=True, timeout=10, check=False)
                        available = probe.returncode == 0 and bool(probe.stdout.strip())
                    except (OSError, subprocess.TimeoutExpired):
                        pass
                packages[package] = available
                if not available:
                    missing.append(package)
        return {"ready": not missing, "missing": missing, "use_tex": self.use_tex,
                "python": python, "executables": executables, "tex_packages": packages}

    def _rc(self):
        dependencies = self.check_dependencies()
        if not dependencies["ready"]:
            message = "Publication rendering dependencies are missing: " + ", ".join(dependencies["missing"]) + "."
            if not all(dependencies["python"].values()):
                message += " Install NumPy and Matplotlib (python -m pip install numpy matplotlib)."
            if self.use_tex:
                message += " Install LaTeX/dvipng and the listed TeX packages, or explicitly use use_tex=False for mathtext."
            raise RuntimeError(message)
        return {"text.usetex": self.use_tex, "text.latex.preamble": r"\usepackage{amsmath}",
                "font.family": "serif", "font.serif": ["Computer Modern Roman"] if self.use_tex else ["DejaVu Serif"],
                "mathtext.fontset": "cm", "font.size": self.style.font_size, "axes.labelsize": self.style.axis_size,
                "xtick.labelsize": self.style.font_size, "ytick.labelsize": self.style.font_size, "legend.fontsize": self.style.legend_size,
                "axes.linewidth": .8, "xtick.direction": "in", "ytick.direction": "in",
                "xtick.top": True, "ytick.right": True, "legend.frameon": False,
                "hatch.linewidth": .5, "savefig.dpi": self.style.dpi, "pdf.fonttype": 42}

    def _draw_heatmap(self, fig, ax, item, index):
        import numpy as np
        from matplotlib.colors import LogNorm, Normalize
        values = np.asarray(item["values"], dtype=float)
        low, high = item["zmin"], item["zmax"]
        if item["log_z"]:
            values = np.ma.masked_less_equal(values, 0)
            low = float(values.min()) if low is None else low
            high = float(values.max()) if high is None else high
            if low == high:
                if item["zmin"] is None: low /= 2
                if item["zmax"] is None: high *= 2
            if low >= high:
                raise ValueError("Logarithmic color limits do not span the visible data")
            norm = LogNorm(low, high)
        else:
            norm = Normalize(low, high)
        mesh = ax.pcolormesh(item["xedges"], item["yedges"], values,
                            norm=norm, cmap=self.style.color_map, shading="flat", rasterized=True)
        option = item.get("draw", "").upper().replace("SAME", "").strip()
        if option != "COL":
            position = ax.get_position()
            pad, width = 6/72/self.layout.width, 6/72/self.layout.width
            cax = fig.add_axes([position.x1+pad, position.y0, width, position.height])
            cax.set_gid(f"panel-{index+1}-colorbar")
            colorbar = fig.colorbar(mesh, cax=cax)
            colorbar.set_label(_text(item["zlabel"]), fontsize=self.style.axis_size, labelpad=3)
            colorbar.ax.tick_params(labelsize=self.style.font_size, length=3, pad=2)
            colorbar.ax.minorticks_off()
        return mesh

    def _draw_ratio(self, ax, ratio, xlabel):
        import numpy as np
        from matplotlib.patches import Rectangle
        from matplotlib.ticker import MaxNLocator
        points = ratio["points"]
        x, y = _values(points, "x"), _values(points, "y")
        if "band" in ratio:
            band = ratio["band"]
            for bx, by, xl, xh, yl, yh in zip(band["x"], band["y"], band["xerror_low"], band["xerror_high"], band["error_low"], band["error_high"]):
                ax.add_patch(Rectangle((bx-xl, by-yl), xl+xh, yl+yh, facecolor="none",
                                       edgecolor=".3", hatch="////", linewidth=0))
        if ratio["unity_line"]:
            ax.axhline(1, color=".4", linestyle="--", linewidth=.8)
        ax.errorbar(x, y, yerr=np.array([points["error_low"], points["error_high"]]),
                    fmt="o", color="black", markersize=self.style.marker_size, elinewidth=.85, capsize=0)
        ax.set_ylim(ratio["ymin"], ratio["ymax"])
        ax.set_ylabel(_text(ratio["ylabel"]), labelpad=3)
        ax.set_xlabel(xlabel, labelpad=4)
        ax.yaxis.set_major_locator(MaxNLocator(nbins=3, prune="both"))
        ax.minorticks_on()
        ax.tick_params(which="major", length=3, pad=3)
        ax.tick_params(which="minor", length=1.5)
        if ratio["arrows"]:
            span = ratio["ymax"]-ratio["ymin"]
            for px, py in zip(x, y):
                if py > ratio["ymax"] or py < ratio["ymin"]:
                    upper = py > ratio["ymax"]
                    boundary = ratio["ymax"] if upper else ratio["ymin"]
                    start = boundary + (-.18 if upper else .18)*span
                    ax.annotate("", xy=(px, boundary), xytext=(px, start),
                                arrowprops={"arrowstyle": "-|>", "color": "black", "lw": .8})

    def _draw(self):
        import numpy as np
        import matplotlib.pyplot as plt
        from matplotlib.lines import Line2D
        from matplotlib.patches import Patch
        from matplotlib.backends.backend_agg import FigureCanvasAgg
        from matplotlib.figure import Figure

        layout = self.layout
        size = (layout.width, layout.height)
        if self.preset and len(self.panels) != layout.rows*layout.columns:
            raise ValueError(f"{self.preset} requires exactly {layout.rows*layout.columns} panels")
        rectangles = layout.rectangles(len(self.panels))
        for panel in self.panels:
            _validate(panel)
        sample = self.panels[0]["sample"]
        if any(panel["sample"] != sample for panel in self.panels[1:]):
            raise ValueError("Panels sharing one publication header must have identical Sample metadata")
        fig = Figure(figsize=size)
        FigureCanvasAgg(fig)
        axes, all_entries, peaks = [], {}, []
        common_y = self.shared_ylabel
        if layout.share_y and common_y is None and len({p["ylabel"] for p in self.panels}) == 1:
            common_y = self.panels[0]["ylabel"]
        same_x = len({p["xlabel"] for p in self.panels}) == 1
        for i, panel in enumerate(self.panels):
            rect = list(rectangles[i])
            heatmap = next((item for item in panel["overlays"] if item["kind"] == "hist2d"), None)
            ratio = panel.get("ratio")
            ratio_ax = None
            if heatmap and heatmap.get("draw", "").upper().replace("SAME", "").strip() != "COL":
                rect[2] *= 1-layout.colorbar_space
            if ratio:
                gap = layout.ratio_gap/72/layout.height
                available = rect[3]-gap
                if available <= 0:
                    raise ValueError("Ratio gap leaves no panel area; increase figure height")
                ratio_height = available*ratio["split"]
                ratio_rect = [rect[0], rect[1], rect[2], ratio_height]
                rect[1] += ratio_height+gap
                rect[3] = available-ratio_height
            ax = fig.add_axes(rect)
            ax.set_gid(f"panel-{i+1}-main")
            if ratio:
                ratio_ax = fig.add_axes(ratio_rect, sharex=ax)
                ratio_ax.set_gid(f"panel-{i+1}-ratio")
            axes.append(ax)
            ax.minorticks_on()
            ax.tick_params(which="major", length=3, pad=3)
            ax.tick_params(which="minor", length=1.5)
            entries, extent = [], []
            def remember(item, handle):
                if item.get("legend", True) and panel["legend_enable"]:
                    signature = (item["label"], item.get("color", {}).__repr__(), item.get("is_data", False), item["kind"])
                    entries.append((item.get("priority", 0), signature, handle))
            bottom = None
            peak, low = 0., 0.
            if heatmap:
                self._draw_heatmap(fig, ax, heatmap, i)
                extent += [heatmap["xedges"][0], heatmap["xedges"][-1]]
            for item in panel["stacks"]:
                values = _values(item, "values")
                edges = _values(item, "edges")
                if bottom is None:
                    bottom = np.zeros_like(values)
                    stack_edges = edges
                if not np.array_equal(edges, stack_edges):
                    raise ValueError("Prepared stack histogram binning differs")
                top = bottom + values
                color = item["color"]
                ax.stairs(top, edges, baseline=bottom, fill=True, facecolor=color["fill"], edgecolor=color["line"], linewidth=.5)
                remember(item, Patch(facecolor=color["fill"], edgecolor=color["line"], linewidth=.5))
                peak, low = max(peak, float(top.max())), min(low, float(top.min()))
                bottom = top
                extent += [edges[0], edges[-1]]
            if "band" in panel:
                band = panel["band"]
                y = _values(band, "y")
                lower, upper = y-_values(band, "error_low"), y+_values(band, "error_high")
                ax.stairs(upper, band["edges"], baseline=lower, fill=True, facecolor="none", edgecolor=".3", hatch="////", linewidth=0)
                peak, low = max(peak, float(upper.max(initial=0))), min(low, float(lower.min(initial=0)))
            for item in panel["overlays"]:
                if item["kind"] == "hist2d": continue
                color = item["color"]
                option = item["draw"].upper().replace("SAME", "").strip()
                hist = item["kind"] == "hist"
                y = _values(item, "values" if hist else "y")
                x = (_values(item,"edges")[1:] + _values(item,"edges")[:-1])/2 if hist else _values(item,"x")
                elo, ehi = _values(item,"error_low"), _values(item,"error_high")
                points = item.get("is_data", False) or "P" in option or "E" in option or (not hist and not option)
                if points:
                    mask = np.ones(len(y),dtype=bool)
                    if hist and item.get("is_data") and not item["zero_error"]:
                        mask &= y != 0
                    xerr = None if hist else np.array([item["xerror_low"],item["xerror_high"]])[:,mask]
                    errors = np.array([elo,ehi])[:,mask] if "E" in option or not option or item.get("is_data") else None
                    handle = ax.errorbar(x[mask],y[mask],yerr=errors,xerr=xerr,fmt="o",markersize=self.style.marker_size,
                                         color=color["marker"],elinewidth=.85,capsize=0,zorder=4)
                    if "L" in option:
                        ax.plot(x,y,color=color["line"],linewidth=self.style.line_width)
                elif hist:
                    handle = ax.stairs(y,item["edges"],color=color["line"],linewidth=self.style.line_width,baseline=None,zorder=3)
                elif option in ("2","3"):
                    handle = ax.fill_between(x,y-elo,y+ehi,color=color["fill"],alpha=.3)
                else:
                    handle, = ax.plot(x,y,color=color["line"],linewidth=self.style.line_width)
                if points:
                    handle = Line2D([], [], marker='o', linestyle='none', color=color['marker'], markersize=self.style.marker_size)
                elif hist:
                    handle = Line2D([], [], color=color['line'], linewidth=self.style.line_width)
                remember(item,handle)
                peak, low = max(peak,float((y+ehi).max(initial=0))), min(low,float((y-elo).min(initial=0)))
                extent += list(item["edges"] if hist else x)
            if "band" in panel and panel["legend_enable"]:
                band = panel["band"]
                entries.append((10**9,(band["label"],"band",False,"band"),Patch(facecolor="none",hatch="////",edgecolor=".3",linewidth=0)))
            entries.sort(key=lambda entry: entry[0])
            for priority, key, handle in entries:
                all_entries.setdefault(key,(priority,handle))
            if not extent:
                raise ValueError("Publication panel has no bins or graph points")
            ax.set_xlim(*(panel.get("xlim") or (min(extent),max(extent))))
            xlabel = "" if layout.share_x and i+layout.columns<len(self.panels) and same_x else _text(panel["xlabel"])
            ax.set_xlabel("" if ratio else xlabel,labelpad=4)
            if ratio:
                ax.tick_params(axis="x", labelbottom=False)
                self._draw_ratio(ratio_ax, ratio, xlabel)
            ylabel = "" if common_y is not None else _text(panel["ylabel"])
            if layout.wrap_ylabel:
                ylabel = ylabel.replace("Candidates per ", "Candidates\nper ")
            ax.set_ylabel(ylabel,labelpad=3,fontsize=layout.ylabel_size or self.style.axis_size)
            ax.xaxis.set_major_locator(plt.MaxNLocator(nbins=3 if layout.compact_ticks else 5,prune="both" if layout.compact_ticks else None))
            if panel["log_y"]:
                ax.set_yscale("log")
            else:
                ax.yaxis.set_major_locator(plt.MaxNLocator(nbins=3 if layout.compact_ticks else 4,integer=True))
            if layout.panel_labels:
                index, label = i, ""
                while index >= 0:
                    label = chr(97+index % 26)+label
                    index = index//26-1
                ax.text(.035,.96,f"({label})",transform=ax.transAxes,va="top",fontweight="bold")
            if layout.legend == "inside" and entries:
                # Matplotlib fills columns first; the exporter order reads across rows.
                cols = layout.legend_columns or max(1,panel["legend_columns"])
                ordered = [entry for col in range(cols) for entry in entries[col::cols]]
                ax.legend([e[2] for e in ordered],[_text(e[1][0]) for e in ordered],loc=layout.legend_location,ncol=cols,
                          handlelength=1.2,columnspacing=1,handletextpad=.4,labelspacing=.3,borderaxespad=.4)
            if heatmap:
                bottom = heatmap["ylim"][0] if panel["ymin"] is None else panel["ymin"]
                top = heatmap["ylim"][1] if panel["ymax"] is None else panel["ymax"]
                if not math.isfinite(bottom) or not math.isfinite(top) or top <= bottom:
                    raise ValueError("TH2 Y limits must be finite and increasing")
                if panel["log_y"] and bottom <= 0:
                    raise ValueError("Logarithmic TH2 Y axis requires a positive lower limit")
                ax.set_ylim(bottom, top)
                # Coordinate axes are continuous, unlike integer event counts.
                if not panel["log_y"]: ax.yaxis.set_major_locator(plt.MaxNLocator(nbins=4))
                peaks.append(None)
            else:
                bottom = panel["ymin"] if panel["ymin"] is not None else (.1 if panel["log_y"] else min(0,low*1.05))
                if panel["log_y"] and bottom<=0:
                    raise ValueError("Logarithmic publication Y minimum must be positive")
                top = max(peak,bottom+1)
                ax.set_ylim(bottom, panel["ymax"] if panel["ymax"] is not None else top*1.18)
                peaks.append((bottom,top))
            for key in ("cut_lower","cut_upper"):
                if panel[key] is not None:
                    ax.axvline(panel[key],color=".35",linestyle="--",linewidth=.8)
        if common_y is not None:
            fig.text(layout.shared_ylabel_x,.52,_text(common_y),rotation=90,fontsize=self.style.axis_size,ha="center",va="center")
        header=[]
        left,right=min(ax.get_position().x0 for ax in axes),max(ax.get_position().x1 for ax in axes)
        top=axes[0].get_position().y1
        if sample["enabled"]:
            experiment = sample["experiment"].replace("\\", "").replace("{", "").replace("}", "")
            title = (r"\textbf{"+_text(experiment)+"}" if self.use_tex else
                     r"$\mathbf{"+experiment.replace(" ", r"\ ")+"}$")+" "+_text(sample["comment"])
            header.append(fig.text(left+.006,.98,title,fontsize=self.style.font_size,va="top"))
            if sample["luminosity"]>0 and sample["comment"].strip().lower()!="simulation":
                unit=sample["luminosity_unit"]
                if unit not in ("fb^{-1}","pb^{-1}"):
                    raise ValueError("Publication luminosity unit must be fb^{-1} or pb^{-1}")
                lumi=rf'$\int L\,\mathrm{{d}}t = {sample["luminosity"]:.0f}\ \mathrm{{{unit[:2]}}}^{{-1}}$'
                header.append(fig.text(right-.006,.98,lumi,fontsize=self.style.font_size,va="top",ha="right"))
        if layout.legend == "shared" and all_entries:
            keys=sorted(all_entries,key=lambda key: all_entries[key][0])
            legend=fig.legend([all_entries[k][1] for k in keys],[_text(k[0]) for k in keys],loc="upper center",
                              bbox_to_anchor=(.53 if layout.inline_legend else (left+right)/2,.99),ncol=layout.legend_columns or len(keys),fontsize=self.style.legend_size,handlelength=1,columnspacing=.65,handletextpad=.35,borderaxespad=0)
            if layout.inline_legend:
                header.append(legend)
            else:
                # A separate shared-legend row leaves the sample header unobstructed.
                fig.canvas.draw()
                height = legend.get_window_extent(fig.canvas.get_renderer()).height/fig.bbox.height
                legend.set_bbox_to_anchor(((left+right)/2, top+self.style.header_gap/72/layout.height+height), transform=fig.transFigure)
                for item in header:
                    text_height = item.get_window_extent(fig.canvas.get_renderer()).height/fig.bbox.height
                    x,y=item.get_position()
                    item.set_position((x,top+self.style.header_gap/72/layout.height+height+4/72/layout.height+text_height))
                header.append(legend)
        fig.canvas.draw()
        renderer=fig.canvas.get_renderer()
        for ax,panel,limits in zip(axes,self.panels,peaks):
            if limits is None: continue
            bottom,peak = limits
            if panel["ymax"] is not None:
                if panel["ymax"]<=bottom:
                    raise ValueError("Publication Y maximum must exceed Y minimum")
                continue
            legend=ax.get_legend()
            fraction=min(.55,(legend.get_window_extent(renderer).height+8)/ax.bbox.height) if legend else .15
            top=bottom*(max(peak/bottom,10))**(1/(1-fraction)) if panel["log_y"] else bottom+(peak-bottom)/(1-fraction)
            ax.set_ylim(bottom,top)
        if header:
            lowest=min(item.get_window_extent(renderer).y0 for item in header)
            offset=(fig.transFigure.transform((0,axes[0].get_position().y1))[1]+self.style.header_gap*fig.dpi/72-lowest)/fig.bbox.height
            for item in header:
                if hasattr(item,"get_position"):
                    x,y=item.get_position(); item.set_position((x,y+offset))
                else:
                    box=item.get_bbox_to_anchor();x,y=fig.transFigure.inverted().transform((box.x0,box.y0))
                    item.set_bbox_to_anchor((x,y+offset),transform=fig.transFigure)
        return fig

    def render(self):
        """Return a standalone Matplotlib Figure; save() keeps export rcParams scoped."""
        rc = self._rc()
        import matplotlib as mpl
        with mpl.rc_context(rc):
            return self._draw()

    def save(self, output, *, dpi=None):
        """Atomically export PDF/PNG/SVG; dpi overrides style.dpi for this save only.

        DPI sets PNG resolution and rasterized layers in vector files. Dependencies
        and options are checked before creating output directories or rendering.
        """
        if dpi is not None and (not math.isfinite(dpi) or dpi <= 0):
            raise ValueError("dpi must be finite and positive")
        output=Path(output)
        kind=output.suffix.lower().lstrip(".")
        if kind not in ("pdf","png","svg"):
            raise ValueError("Publication output must have a .pdf, .png, or .svg extension")
        rc = self._rc()
        if dpi is not None:
            rc["savefig.dpi"] = dpi
        import matplotlib as mpl
        import matplotlib.pyplot as plt
        output.parent.mkdir(parents=True,exist_ok=True)
        temporary=None
        fig=None
        try:
            with mpl.rc_context(rc):
                fig=self._draw()
                fd,temporary=tempfile.mkstemp(prefix=".cascade-publication-",suffix="."+kind,dir=output.parent)
                os.close(fd)
                fig.savefig(temporary,format=kind)
                os.replace(temporary,output)
                temporary=None
        finally:
            if fig is not None: plt.close(fig)
            if temporary is not None: Path(temporary).unlink(missing_ok=True)
        return output


def main():
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input",required=True)
    parser.add_argument("--output",required=True)
    parser.add_argument("--layout",choices=_LAYOUTS,default="single")
    parser.add_argument("--no-tex",action="store_true")
    parser.add_argument("--dpi",type=float,default=None,help="Override export resolution (style default: 220 dpi)")
    args=parser.parse_args()
    with open(args.input,encoding="utf-8") as stream:
        document=json.load(stream)
    if document.get("schema_version")!=1:
        raise ValueError("Unsupported publication document schema")
    options = document.get("options", {})
    layout = PublicationLayout(**options["layout"]) if "layout" in options else args.layout
    style = PublicationStyle(**options.get("style", {}))
    plotter=PublicationFigure(layout, style=style, use_tex=False if args.no_tex else None)
    for panel in document["panels"]:plotter.add(panel)
    plotter.save(args.output, dpi=args.dpi)


if __name__=="__main__":
    main()
