"""Regression coverage for the common NumPy/publication rendering path."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

AVAILABLE = all(importlib.util.find_spec(name) for name in ("numpy", "matplotlib"))


@unittest.skipUnless(AVAILABLE, "NumPy and Matplotlib are optional")
class PublicationTests(unittest.TestCase):
    def test_export_dpi_override_and_style_default_are_scoped(self):
        import matplotlib as mpl
        import matplotlib.pyplot as plt
        from PIL import Image
        from cascade import PublicationFigure, PublicationStyle, histogram_panel
        figure = PublicationFigure(style=PublicationStyle(use_tex=False, dpi=300)).add(
            histogram_panel([.2, .4], bins=[0, 1]))
        plt.get_backend()
        before = mpl.rcParams.copy()
        with tempfile.TemporaryDirectory() as directory:
            for name, kwargs, expected in (("default", {}, 300), ("override", {"dpi": 600}, 600),
                                           ("default-again", {}, 300)):
                path = figure.save(Path(directory) / f"{name}.png", **kwargs)
                with Image.open(path) as image:
                    self.assertAlmostEqual(image.info["dpi"][0], expected, delta=.1)
        self.assertEqual(mpl.rcParams, before)

    def test_invalid_dpi_and_render_failure_preserve_existing_output(self):
        from cascade import PublicationFigure, histogram_panel
        from matplotlib.figure import Figure
        figure = PublicationFigure(use_tex=False).add(histogram_panel([.2], bins=[0, 1]))
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "plot.png"
            output.write_bytes(b"existing")
            for dpi in (0, -1, float("nan"), float("inf")):
                with self.subTest(dpi=dpi), self.assertRaises(ValueError):
                    figure.save(output, dpi=dpi)
            with mock.patch.object(Figure, "savefig", side_effect=OSError("export failed")), self.assertRaises(OSError):
                figure.save(output, dpi=600)
            self.assertEqual(output.read_bytes(), b"existing")
            self.assertEqual(list(Path(directory).iterdir()), [output])

    def test_tex_preflight_reports_tools_and_packages_before_render(self):
        from cascade import PublicationFigure, histogram_panel
        import subprocess
        figure = PublicationFigure().add(histogram_panel([.2], bins=[0, 1]))
        def which(name):
            return None if name == "dvipng" else "/bin/" + name
        def probe(command, **kwargs):
            found = command[-1] != "type1ec.sty"
            return subprocess.CompletedProcess(command, 0 if found else 1, "/tex/file.sty" if found else "", "")
        with mock.patch("cascade.publication.shutil.which", side_effect=which), \
             mock.patch("cascade.publication.subprocess.run", side_effect=probe), \
             mock.patch.object(figure, "_draw") as draw:
            report = figure.check_dependencies()
            self.assertFalse(report["ready"])
            self.assertEqual(report["missing"], ["dvipng", "type1ec.sty"])
            with tempfile.TemporaryDirectory() as directory:
                output = Path(directory) / "new-directory" / "plot.pdf"
                with self.assertRaisesRegex(RuntimeError, "dvipng.*type1ec.sty.*use_tex=False"):
                    figure.save(output)
                self.assertFalse(output.parent.exists())
            draw.assert_not_called()
        fallback = PublicationFigure(use_tex=False)
        with mock.patch("cascade.publication.shutil.which") as tools:
            self.assertTrue(fallback.check_dependencies()["ready"])
            tools.assert_not_called()

    def test_preflight_reports_missing_python_dependencies(self):
        from cascade import PublicationFigure
        figure = PublicationFigure(use_tex=False)
        with mock.patch("cascade.publication.importlib.util.find_spec", return_value=None):
            self.assertEqual(figure.check_dependencies()["missing"], ["numpy", "matplotlib"])
            with self.assertRaisesRegex(RuntimeError, "pip install numpy matplotlib"):
                figure.render()

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
