import pathlib
import runpy
import subprocess
import tempfile
import unittest
from unittest.mock import patch

SCRIPTS = pathlib.Path(__file__).resolve().parents[1] / "scripts"
prune_runtime = runpy.run_path(str(SCRIPTS / "runtime_layout.py"))["prune_runtime"]
stage_runtime = runpy.run_path(str(SCRIPTS / "stage_runtime.py"))["stage_runtime"]


class RuntimeLayoutTests(unittest.TestCase):
    def test_pruning_removes_stale_files_without_following_symlinks(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            runtime = root / "runtime"
            runtime.mkdir()
            keep = runtime / "current.py"
            keep.write_text("current")
            outside = root / "external"
            outside.mkdir()
            (outside / "keep.py").write_text("external")
            (runtime / "stale-link").symlink_to(outside, target_is_directory=True)
            (runtime / "_cascade.so").symlink_to(outside / "keep.py")
            (runtime / "__pycache__").mkdir()
            (runtime / "__pycache__" / "removed.pyc").write_bytes(b"old")
            (runtime / "removed.py").write_text("old")
            prune_runtime(runtime, [keep, runtime / "_cascade.so"])
            self.assertEqual(sorted(p.name for p in runtime.iterdir()), ["_cascade.so", "current.py"])
            self.assertEqual((outside / "keep.py").read_text(), "external")
            prune_runtime(runtime, [keep, runtime / "_cascade.so"])

    def test_pruning_rejects_symlink_root_and_external_expected_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "link").symlink_to(root, target_is_directory=True)
            with self.assertRaises(ValueError):
                prune_runtime(root / "link", [])
            with self.assertRaises(ValueError):
                prune_runtime(root, [root.parent / "outside.py"])

    def test_staging_uses_empty_prefix_and_preserves_existing_destination(self):
        with tempfile.TemporaryDirectory() as directory:
            output = pathlib.Path(directory) / "release"

            def install(command, **kwargs):
                prefix = pathlib.Path(next(arg[7:] for arg in command if arg.startswith("PREFIX=")))
                self.assertFalse(prefix.exists())
                prefix.mkdir()
                (prefix / "current.py").write_text("installed")

            with patch("subprocess.run", side_effect=install) as run:
                stage_runtime(output)
                self.assertEqual((output / "current.py").read_text(), "installed")
                with self.assertRaises(FileExistsError):
                    stage_runtime(output)
                self.assertEqual(run.call_count, 1)
            self.assertEqual(list(output.parent.iterdir()), [output])

    def test_failed_install_does_not_publish_partial_runtime(self):
        with tempfile.TemporaryDirectory() as directory:
            output = pathlib.Path(directory) / "release"
            with patch("subprocess.run", side_effect=subprocess.CalledProcessError(1, "scons")):
                with self.assertRaises(subprocess.CalledProcessError):
                    stage_runtime(output)
            self.assertEqual(list(output.parent.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
