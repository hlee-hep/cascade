"""Regression coverage for the common NumPy/publication rendering path."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

AVAILABLE = all(importlib.util.find_spec(name) for name in ("numpy", "matplotlib"))


@unittest.skipUnless(AVAILABLE, "NumPy and Matplotlib are optional")
class PublicationTests(unittest.TestCase):
    def test_common_binning_covers_disjoint_samples(self):
        from cascade import histogram_panel
        panel = histogram_panel([0, 1], backgrounds=[[10, 11]], bins=2)
        self.assertEqual(panel["overlays"][0]["edges"], [0, 5.5, 11])
        self.assertEqual(panel["stacks"][0]["values"], [0, 2])
        self.assertEqual(panel["overlays"][0]["values"], [2, 0])
        json.dumps(panel, allow_nan=False)

    def test_weighted_ratio_and_empty_denominator(self):
        import numpy as np
        from cascade import histogram_panel
        panel = histogram_panel([0.2, 0.4, 1.2], weights=[2, 3, 4],
                                backgrounds=[[0.1, 0.3]], background_weights=[[1, 2]],
                                bins=[0, 1, 2], ratio=True)
        np.testing.assert_allclose(panel["overlays"][0]["error_low"], [np.sqrt(13), 4])
        ratio = panel["ratio"]
        np.testing.assert_allclose(ratio["points"]["x"], [0.5])
        np.testing.assert_allclose(ratio["points"]["y"], [5/3])
        np.testing.assert_allclose(ratio["points"]["error_low"], [np.sqrt(13)/3])
        np.testing.assert_allclose(ratio["band"]["error_low"], [np.sqrt(5)/3])
        json.dumps(panel, allow_nan=False)

    def test_density_integrates_data_and_total_stack_to_one(self):
        import numpy as np
        from cascade import histogram_panel
        panel = histogram_panel([0.5, 2], backgrounds=[[0.5], [2, 2]],
                                bins=[0, 1, 3], density=True)
        self.assertAlmostEqual(np.dot(panel["overlays"][0]["values"], [1, 2]), 1)
        total = np.sum([item["values"] for item in panel["stacks"]], axis=0)
        self.assertAlmostEqual(np.dot(total, [1, 2]), 1)

    def test_invalid_inputs_are_rejected(self):
        from cascade import histogram_panel
        cases = [dict(), dict(data=[float("nan")]), dict(data=[1], weights=[1, 2]),
                 dict(backgrounds=[[1]], labels=[]), dict(data=[1], ratio=True),
                 dict(data=[1], bins=[0, 0, 2]), dict(data=[], density=True),
                 dict(data=[1], backgrounds=[[1]], ratio=True, ratio_ylim=(2, 1))]
        for kwargs in cases:
            with self.subTest(kwargs=kwargs), self.assertRaises(ValueError):
                histogram_panel(**kwargs)

    def test_render_and_save_share_the_publication_backend(self):
        import matplotlib as mpl
        import matplotlib.pyplot as plt
        from cascade import PublicationFigure, histogram_panel
        plt.get_backend()  # Resolve Matplotlib's lazy backend before the snapshot.
        before = mpl.rcParams.copy()
        panel = histogram_panel([0.2, 1.2], backgrounds=[[0.1, 1.1]], bins=[0, 1, 2], ratio=True)
        publication = PublicationFigure(use_tex=False).add(panel)
        panel["overlays"][0]["values"][0] = 999
        self.assertEqual(publication.panels[0]["overlays"][0]["values"][0], 1)
        figure = publication.render()
        self.assertEqual([axis.get_gid() for axis in figure.axes], ["panel-1-main", "panel-1-ratio"])
        plt.close(figure)
        with tempfile.TemporaryDirectory() as directory:
            for extension in ("pdf", "png", "svg"):
                output = publication.save(Path(directory) / f"plot.{extension}")
                self.assertGreater(output.stat().st_size, 100)
        self.assertEqual(mpl.rcParams, before)


if __name__ == "__main__":
    unittest.main()
