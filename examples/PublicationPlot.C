#include "PlotManager.hh"
#include <TH1D.h>

// Activate Cascade with its Python dependencies before running this macro.
void PublicationPlot()
{
    TH1D background("publication_background", "", 24, -0.3, 0.3);
    TH1D data("publication_data", "", 24, -0.3, 0.3);
    for (int i = 0; i < 120; ++i)
    {
        background.Fill(-0.29 + (i % 24) * 0.025, 0.8);
        data.Fill(-0.29 + (i % 24) * 0.025);
    }
    auto spec = PlotSpec::Simple()
                    .X("#Delta E [GeV]").Y("Candidates / bin")
                    .Stack({StackItemSpec(&background, "Background", ColorSpec(1, kAzure - 9, 1))})
                    .Overlay({OverlaySpec::Hist(&data, "Data", ColorSpec(), DrawSpec(), true)});
    spec.Sample.Comment = "Preliminary";
    spec.Sample.Lumi = 428;
    spec.Legend.NCol = 2;
    PlotManager manager;
    manager.SavePublication({spec}, "publication_single.pdf", "single");
    manager.SavePublication({spec, spec}, "publication_vertical.pdf", "vertical2");
    manager.SavePublication({spec, spec, spec, spec}, "publication_row.pdf", "row4");

    PublicationOptions options;
    options.Layout.Rows = 2;
    options.Layout.Columns = 3;
    options.Layout.Width = 7;
    options.Layout.Height = 4.6;
    options.Layout.Left = .08;
    options.Layout.Right = .025;
    options.Layout.Bottom = .12;
    options.Layout.Top = .16;
    options.Layout.HGap = .055;
    options.Layout.VGap = .16;
    options.Layout.Legend = "shared";
    options.Layout.ShareX = true;
    options.Style.FontSize = 10;
    options.Style.AxisSize = 11;
    options.Style.LegendSize = 10;
    manager.SavePublication({spec, spec, spec, spec, spec}, "publication_grid.pdf", options);
}
