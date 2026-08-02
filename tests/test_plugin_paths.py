import importlib.util
import json
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


if __name__ == "__main__":
    unittest.main()
