import importlib.util
import json
import multiprocessing
import os
import pathlib
import tempfile
import unittest
from unittest import mock


def _load_plugin_paths():
    path = pathlib.Path(__file__).parents[1] / "python" / "plugin_paths.py"
    spec = importlib.util.spec_from_file_location("cascade_test_plugin_paths", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


plugin_paths = _load_plugin_paths()


def _concurrent_add(config, prefix, ready, start):
    module = _load_plugin_paths()
    os.environ["CASCADE_CONFIG_FILE"] = config
    ready.put(True)
    start.wait(10)
    module.add_plugin_prefix(prefix)


class PluginPathConfigTests(unittest.TestCase):
    def test_prefixes_are_persisted_deduplicated_and_removed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            config = root / "config" / "config.json"
            prefix = root / "plugins"
            prefix.mkdir()
            with mock.patch.dict(os.environ, {"CASCADE_CONFIG_FILE": str(config)}, clear=False):
                self.assertTrue(plugin_paths.add_plugin_prefix(str(prefix)))
                self.assertFalse(plugin_paths.add_plugin_prefix(str(prefix / ".")))
                self.assertEqual(plugin_paths.configured_plugin_prefixes(), [str(prefix.resolve())])
                document = json.loads(config.read_text(encoding="utf-8"))
                self.assertEqual(document["schema"], 1)
                self.assertEqual(
                    document["plugin_prefixes"],
                    [{"enabled": True, "path": str(prefix.resolve())}],
                )
                self.assertTrue(plugin_paths.remove_plugin_prefix(str(prefix)))
                self.assertFalse(plugin_paths.remove_plugin_prefix(str(prefix)))
                self.assertEqual(plugin_paths.configured_plugin_prefixes(), [])

    def test_invalid_config_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            config = pathlib.Path(directory) / "config.json"
            config.write_text('{"schema": 99, "plugin_prefixes": []}', encoding="utf-8")
            with self.assertRaises(RuntimeError):
                plugin_paths.load_config(str(config))

    def test_layout_derives_all_runtime_roots_from_one_prefix(self):
        layout = plugin_paths.plugin_layout("/opt/cascade-plugins")
        self.assertEqual(layout["cpp"], "/opt/cascade-plugins/lib/cascade/plugin")
        self.assertEqual(layout["python"], "/opt/cascade-plugins/lib/cascade/pyplugin")
        self.assertEqual(layout["trust_store"], "/opt/cascade-plugins/share/cascade/trusted_keys")

    def test_config_path_uses_userprofile_when_home_is_absent(self):
        with tempfile.TemporaryDirectory() as directory:
            environment = {"USERPROFILE": directory}
            with mock.patch.dict(os.environ, environment, clear=True):
                self.assertEqual(
                    plugin_paths.config_path(),
                    str(pathlib.Path(directory) / ".config" / "cascade" / "config.json"),
                )

    def test_symlinked_config_parent_is_rejected(self):
        if os.name == "nt":
            self.skipTest("symlink creation requires platform-specific privileges on Windows")
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            real_parent = root / "real"
            linked_parent = root / "linked"
            real_parent.mkdir()
            linked_parent.symlink_to(real_parent, target_is_directory=True)
            with self.assertRaisesRegex(RuntimeError, "symbolic link"):
                plugin_paths.add_plugin_prefix(
                    str(root), str(linked_parent / "cascade" / "config.json")
                )

    def test_concurrent_config_writers_preserve_both_prefixes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            config = root / "config" / "config.json"
            prefixes = [root / "one", root / "two"]
            for prefix in prefixes:
                prefix.mkdir()
            ready = multiprocessing.Queue()
            start = multiprocessing.Event()
            processes = [
                multiprocessing.Process(
                    target=_concurrent_add,
                    args=(str(config), str(prefix), ready, start),
                )
                for prefix in prefixes
            ]
            for process in processes:
                process.start()
            for _ in processes:
                self.assertTrue(ready.get(timeout=10))
            start.set()
            for process in processes:
                process.join(10)
                self.assertEqual(process.exitcode, 0)
            self.assertEqual(
                set(plugin_paths.configured_plugin_prefixes(str(config))),
                {str(prefix.resolve()) for prefix in prefixes},
            )


if __name__ == "__main__":
    unittest.main()
