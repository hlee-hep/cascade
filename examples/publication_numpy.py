"""Render NumPy events through the shared publication backend, without PyROOT."""
from pathlib import Path
import sys

import numpy as np
from cascade import PublicationFigure, histogram_panel

rng = np.random.default_rng(42)
panel = histogram_panel(
    rng.normal(size=400),
    backgrounds=[rng.normal(size=1000)],
    background_weights=[np.full(1000, 0.4)],
    labels=["Background"],
    bins=24,
    hist_range=(-3, 3),
    ratio=True,
    xlabel="X",
    sample={"enabled": True, "experiment": "Belle II", "comment": "Simulation"},
)
output = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("publication_numpy.pdf")
PublicationFigure(use_tex=False).add(panel).save(output)
