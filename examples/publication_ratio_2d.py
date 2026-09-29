"""Synthetic ROOT ratio/TH2 example; run inside the updated Cascade runtime.

No paper data or fit results are used. ROOT owns all numerical transformations.
"""
from array import array
import argparse
import json
import math
from pathlib import Path
import sys

import ROOT
from cascade import PublicationFigure, PublicationLayout, PublicationStyle


def build_specs():
    """Return specs and borrowed objects that must live until snapshot export."""
    ROOT.gROOT.SetBatch(True)
    ROOT.gSystem.Load("libPlotManager")
    mc = ROOT.TH1D("publication_ratio_mc", "", 16, -4, 4)
    data = ROOT.TH1D("publication_ratio_data", "", 16, -4, 4)
    for bin_index in range(1, 17):
        x = mc.GetBinCenter(bin_index)
        count = int(45*math.exp(-x*x/4)+4)
        for _ in range(count): mc.Fill(x, .8)
        for _ in range(max(0, int(count*.8)+(bin_index % 5)-2)): data.Fill(x)
    # An undefined denominator and an off-scale ratio exercise both policies.
    mc.SetBinContent(2, 0)
    data.SetBinContent(15, 30)
    ratio = ROOT.PlotSpec()
    ratio.X("q").Y("Candidates / bin").UseRatio(True)
    ratio.Sample.Comment = "Simulation"
    ratio.Layout.RatioSplit = .30
    ratio.Ratio.YMin = .2
    ratio.Ratio.YMax = 1.8
    ratio.Stacks.push_back(ROOT.StackItemSpec(mc, "Reference", ROOT.ColorSpec(1, ROOT.kAzure-9, 1)))
    ratio.Overlays.push_back(ROOT.OverlaySpec.Hist(data, "Sample", ROOT.ColorSpec(), ROOT.DrawSpec(), True))
    ratio.Ratio.YLabel = "Sample / ref."

    xedges = array('d', [1.4 + .6*(i/32)**1.25 for i in range(33)])
    yedges = array('d', [-.8 + 1.4*(i/28)**1.15 for i in range(29)])
    density = ROOT.TH2D("publication_density", "", 32, xedges, 28, yedges)
    density.GetZaxis().SetTitle("Weighted events")
    for ix in range(1, 33):
        for iy in range(1, 29):
            x, y = density.GetXaxis().GetBinCenter(ix), density.GetYaxis().GetBinCenter(iy)
            value = 80*math.exp(-((x-1.78)/.06)**2/2-((y+.05)/.16)**2/2)
            value = value if value > .05 else 0.
            density.SetBinContent(ix, iy, value)
            density.SetBinError(ix, iy, math.sqrt(value))
    density.SetMinimum(.05)
    density.SetMaximum(100)
    boundary = ROOT.TGraph(121)
    for i in range(121):
        angle = 2*math.pi*i/120
        boundary.SetPoint(i, 1.78+.12*math.cos(angle), -.05+.32*math.sin(angle))
    plane = ROOT.PlotSpec()
    plane.X("M [GeV/c^{2}]").Y("#Delta E [GeV]")
    plane.Sample.Comment = "Simulation"
    plane.Theme.LogZ = True
    draw = ROOT.DrawSpec(); draw.DrawOpt = "COLZ"
    plane.Overlays.push_back(ROOT.OverlaySpec.Hist(density, "Density", ROOT.ColorSpec(), draw))
    curve = ROOT.DrawSpec(); curve.DrawOpt = "L"
    plane.Overlays.push_back(ROOT.OverlaySpec.Graph(boundary, "Region", ROOT.ColorSpec(ROOT.kRed, 0, ROOT.kRed), curve))
    return ratio, plane, (mc, data, density, boundary)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path("publication_ratio_2d"))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    ratio, plane, keepalive = build_specs()
    manager = ROOT.PlotManager()
    documents = [json.loads(str(manager.ExportPublication(spec))) for spec in (ratio, plane)]
    layout = PublicationLayout(columns=2, width=7, height=3.8, left=.08,
                               right=.025, bottom=.17, top=.14, hgap=.11, share_y=False)
    plot = PublicationFigure(layout, style=PublicationStyle())
    for document in documents: plot.add(document)
    plot.save(args.output / "ratio_and_2d.pdf")
    plot.save(args.output / "ratio_and_2d.png")
    options = ROOT.PublicationOptions()
    options.Layout.Width = 3.8
    options.Layout.Height = 3.8
    options.Layout.Bottom = .17
    manager.SavePublication([ratio], str(args.output / "ratio.pdf"), options, sys.executable)
    manager.SavePublication([plane], str(args.output / "density.pdf"), options, sys.executable)
    (args.output / "snapshots.json").write_text(json.dumps(documents, indent=2)+"\n")
    print(f"Saved ratio and TH2 examples to {args.output}")


if __name__ == "__main__":
    main()
