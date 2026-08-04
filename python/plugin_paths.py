"""Compatibility wrappers for the C++ plugin path and configuration service."""

from cascade._cascade import PluginPaths


CONFIG_SCHEMA = 1


def config_path():
    return PluginPaths.config_path()


def canonical_prefix(path):
    return PluginPaths.canonical_prefix(path)


def load_config(path=None):
    return PluginPaths.load_config(path or "")


def configured_plugin_prefixes(path=None):
    return list(PluginPaths.configured_prefixes(path or ""))


def add_plugin_prefix(prefix, path=None):
    return PluginPaths.add_prefix(prefix, path or "")


def remove_plugin_prefix(prefix, path=None):
    return PluginPaths.remove_prefix(prefix, path or "")


def plugin_layout(prefix):
    layout = PluginPaths.layout(prefix)
    return {
        "prefix": layout.prefix,
        "cpp": layout.cpp,
        "python": layout.python,
        "include": layout.include,
        "trust_store": layout.trust_store,
    }


def unique_paths(paths):
    return list(PluginPaths.unique(paths))
