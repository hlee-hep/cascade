# pltPlotManager.py
# Minimal-yet-HEP matplotlib plot manager with Pearson (sqrt(N)) errors
# - plot_hist: single histogram
# - plot_stack: stacked backgrounds (auto palette from experiment preset)
# - compare_data_mc: Data vs MC + MC stat band + ratio panel
#   * Data errors: Pearson sqrt(N) symmetric (if unweighted), else sqrt(sumw2)
#   * MC band: sqrt(sumw2) symmetric; ratio band uses relative sqrt(sumw2)/N
# - set_experiment: CMS/ATLAS/BelleII preset (label, lumi format, palette)
# - save_all: save all figures

import os
import numpy as np
import matplotlib.pyplot as plt
from typing import Optional, Sequence, Tuple

HEP_STYLES = {
    "BelleII": {
        "label": "Belle II",
        "comment": "Preliminary",  # or "Simulation"
        "lumi_fmt": "L = {lumi:.1f} fb$^{{-1}}$ @ $\\Upsilon(4S)$",
        "color_cycle": ["#4477AA", "#EE6677", "#228833", "#CCBB44", "#66CCEE"],
    },
}

class pltPlotManager:
    def __init__(self, outdir: str = "plots", ext: str = "pdf",
                 experiment: Optional[str] = None, lumi: float = 0.0):
        self.outdir = outdir
        self.ext = ext
        os.makedirs(outdir, exist_ok=True)
        self.figures = []  # type: list[plt.Figure]
        self.exp_style = None
        self.lumi = float(lumi)
        if experiment:
            self.set_experiment(experiment, lumi)
        plt.rcParams.update({
            "font.size": 12,
            "axes.labelsize": 13,
            "axes.titlesize": 14,
            "legend.frameon": False,
            "figure.dpi": 120,
        })

    def set_experiment(self, name: str, lumi: float = 0.0, comment: Optional[str] = None):
        if name not in HEP_STYLES:
            raise ValueError(f"Unknown experiment style: {name}")
        self.exp_style = HEP_STYLES[name].copy()
        if comment is not None:
            self.exp_style["comment"] = comment
        self.lumi = float(lumi)
        plt.rcParams["axes.prop_cycle"] = plt.cycler(color=self.exp_style["color_cycle"])

    def _draw_label(self, ax: plt.Axes):
        if not self.exp_style:
            return
        label = f"{self.exp_style['label']} {self.exp_style['comment']}".strip()
        ax.text(0.02, 0.95, label, transform=ax.transAxes,
                ha="left", va="top", fontsize=13, weight="bold")
        if self.lumi > 0:
            fmt = self.exp_style.get("lumi_fmt", "L = {lumi:.1f} fb$^{{-1}}$")
            ax.text(0.98, 0.95, fmt.format(lumi=self.lumi),
                    transform=ax.transAxes, ha="right", va="top", fontsize=11)

    @staticmethod
    def _hist_sumw_sumw2(x: np.ndarray, bins: int, rng: Optional[Tuple[float, float]],
                         w: Optional[np.ndarray] = None) -> Tuple[np.ndarray, np.ndarray, np.ndarray]:
        if w is None:
            sumw, edges = np.histogram(x, bins=bins, range=rng)
            sumw = sumw.astype(float)
            sumw2 = sumw.copy()
        else:
            sumw, edges = np.histogram(x, bins=bins, range=rng, weights=w)
            sw2, _ = np.histogram(x, bins=bins, range=rng, weights=w*w)
            sumw2 = sw2.astype(float)
            sumw = sumw.astype(float)
        return sumw, sumw2, edges

    def plot_hist(self, data: np.ndarray, bins: int = 50, hist_range: Optional[Tuple[float,float]] = None,
                  label: Optional[str] = None, color: Optional[str] = None,
                  histtype: str = "step", density: bool = False,
                  weights: Optional[np.ndarray] = None, logy: bool = False,
                  xlabel: str = "X", ylabel: str = "Events"):
        fig, ax = plt.subplots()
        ax.hist(data, bins=bins, range=hist_range, label=label, color=color,
                histtype=histtype, density=density, weights=weights)
        if label:
            ax.legend()
        ax.set_xlabel(xlabel)
        ax.set_ylabel(ylabel)
        if logy:
            ax.set_yscale("log")
        self._draw_label(ax)
        self.figures.append(fig)
        return fig, ax

    def plot_stack(self, hists: Sequence[np.ndarray], bins: int = 50,
                   labels: Optional[Sequence[str]] = None,
                   colors: Optional[Sequence[str]] = None,
                   hist_range: Optional[Tuple[float,float]] = None,
                   weights: Optional[Sequence[np.ndarray]] = None,
                   logy: bool = False, xlabel: str = "X", ylabel: str = "Events"):
        fig, ax = plt.subplots()
        if labels is None:
            labels = [f"h{i}" for i in range(len(hists))]
        if colors is None:
            colors = plt.rcParams["axes.prop_cycle"].by_key()["color"]
        ax.hist(hists, bins=bins, range=hist_range, stacked=True,
                label=labels, color=colors[:len(hists)], align="mid",
                weights=weights)
        ax.set_xlabel(xlabel)
        ax.set_ylabel(ylabel)
        if logy:
            ax.set_yscale("log")
        ax.legend()
        self._draw_label(ax)
        self.figures.append(fig)
        return fig, ax

    def compare_data_mc(self, data: np.ndarray, mc: np.ndarray, bins: int = 50,
                        range: Optional[Tuple[float,float]] = None,
                        label_data: str = "Data", label_mc: str = "MC",
                        color_data: str = "black", color_mc: Optional[str] = None,
                        with_ratio: bool = True, ratio_ylim: Tuple[float,float] = (0.5, 1.5),
                        data_weights: Optional[np.ndarray] = None,
                        mc_weights: Optional[np.ndarray] = None,
                        logy: bool = False, xlabel: str = "X", ylabel: str = "Events"):
        if with_ratio:
            fig, (ax, rax) = plt.subplots(
                2, 1, sharex=True, figsize=(6.4, 6.4),
                gridspec_kw={"height_ratios": [3, 1], "hspace": 0.05}
            )
        else:
            fig, ax = plt.subplots(figsize=(6.4, 4.8))
            rax = None

        mc_sumw, mc_sumw2, edges = self._hist_sumw_sumw2(mc, bins, range, mc_weights)
        data_sumw, data_sumw2, _ = self._hist_sumw_sumw2(data, bins, range, data_weights)

        centers = 0.5 * (edges[1:] + edges[:-1])
        width   = edges[1] - edges[0]

        if color_mc is None:
            color_mc = plt.rcParams["axes.prop_cycle"].by_key()["color"][0]

        ax.bar(centers, mc_sumw, width=width, align="center",
               label=label_mc, color=color_mc, alpha=0.75, linewidth=0)

        mc_sigma = np.sqrt(np.clip(mc_sumw2, 0.0, None))
        ax.fill_between(
            centers, mc_sumw - mc_sigma, mc_sumw + mc_sigma,
            step="mid", color="gray", alpha=0.30, label="MC stat"
        )

        if data_weights is None:
            yerr = np.sqrt(np.clip(data_sumw, 0.0, None))
        else:
            yerr = np.sqrt(np.clip(data_sumw2, 0.0, None))

        ax.errorbar(centers, data_sumw, yerr=yerr,
                    fmt="o", color=color_data, label=label_data, ms=4, capsize=0)
        ax.set_ylabel(ylabel)
        if logy:
            ax.set_yscale("log")
        ax.legend(loc="best")
        self._draw_label(ax)

        if with_ratio and rax is not None:
            den = np.where(mc_sumw > 0, mc_sumw, np.nan)
            ratio = data_sumw / den

            if data_weights is None:
                r_yerr = np.where(np.isfinite(den), np.sqrt(data_sumw)/den, np.nan)
            else:
                r_yerr = np.where(np.isfinite(den), np.sqrt(data_sumw2)/den, np.nan)

            rax.axhline(1.0, color="gray", linestyle="--", linewidth=1)
            rax.errorbar(centers, ratio, yerr=r_yerr,
                         fmt="o", color=color_data, ms=3, capsize=0)

            with np.errstate(divide='ignore', invalid='ignore'):
                rel = np.where(mc_sumw > 0, mc_sigma / mc_sumw, np.nan)
            mask = np.isfinite(rel)
            rax.fill_between(
                centers[mask], (1 - rel[mask]), (1 + rel[mask]), step="mid",
                color="gray", alpha=0.30
            )

            rax.set_ylabel("Data/MC")
            rax.set_ylim(*ratio_ylim)
            rax.set_xlabel(xlabel)
        else:
            ax.set_xlabel(xlabel)

        self.figures.append(fig)
        return fig, ax, rax

    def save_all(self, prefix: str = "plot"):
        for i, fig in enumerate(self.figures):
            fname = os.path.join(self.outdir, f"{prefix}_{i}.{self.ext}")
            fig.tight_layout()
            fig.savefig(fname)
            print(f"[pltPlotManager] Saved {fname}")
