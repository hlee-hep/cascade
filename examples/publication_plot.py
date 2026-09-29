"""Run in an activated Cascade + PyROOT runtime with Matplotlib and LaTeX."""
import ROOT
from cascade import PublicationFigure, PublicationLayout, PublicationStyle

ROOT.gROOT.SetBatch(True)
ROOT.gSystem.Load('libPlotManager')
background = ROOT.TH1D('pub_background', '', 24, -0.3, 0.3)
data = ROOT.TH1D('pub_data', '', 24, -0.3, 0.3)
for index in range(120):
    background.Fill(-0.29 + (index % 24)*0.025, 0.8)
    data.Fill(-0.29 + (index % 24)*0.025)

spec = ROOT.PlotSpec()
spec.X(r'$\Delta E$ ($\mathrm{GeV}$)').Y('Candidates / bin')
spec.Sample.Comment = 'Preliminary'
spec.Sample.Lumi = 428
spec.Legend.NCol = 2
spec.Stacks.push_back(ROOT.StackItemSpec(background, 'Background', ROOT.ColorSpec(1, ROOT.kAzure-9, 1)))
spec.Overlays.push_back(ROOT.OverlaySpec.Hist(data, 'Data', ROOT.ColorSpec(), ROOT.DrawSpec(), True))

# The inputs can be deleted after add(): the publication owns a numerical snapshot.
figure = PublicationFigure('single').add(spec)
figure.save('publication_single.pdf')

# The C++ entry point uses the same renderer; no intermediate files to manage.
# pm.SavePublication({spec}, "publication_single.pdf", "single");

# A different arrangement uses the same style and source specifications.
layout = PublicationLayout(rows=2, columns=3, width=7, height=4.6,
                           left=.08, right=.025, bottom=.12, top=.16,
                           hgap=.055, vgap=.16, legend="shared", share_x=True)
style = PublicationStyle(font_size=10, axis_size=11, legend_size=10)
grid = PublicationFigure(layout, style=style)
for panel in (spec, spec, spec, spec, spec):
    grid.add(panel)
grid.save("publication_grid.pdf")
