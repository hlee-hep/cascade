import os, sys, hashlib, json
from pathlib import Path
from SCons.Script import Environment, Glob

def hash_file(path):
    if not os.path.exists(path):
        return ""
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()[:12]

def hash_tree(paths):
    h = hashlib.sha256()
    for p in sorted(paths):
        if os.path.exists(p):
            h.update(hash_file(p).encode())
            h.update(p.encode())
    return h.hexdigest()[:12]

def patch_file(input_path, output_path, substitutions):
    if not os.path.exists(input_path):
        return
    content = Path(input_path).read_text()
    for k, v in substitutions.items():
        content = content.replace(k, v)
    Path(output_path).parent.mkdir(parents=True, exist_ok=True)
    Path(output_path).write_text(content)

def read_text(path):
    try:
        return Path(path).read_text()
    except Exception:
        return ""

def write_text(path, content):
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text(content)

# -------- config --------
def require_env(name, default=None):
    value = os.environ.get(name, default)
    if not value:
        raise RuntimeError(f"Missing required environment variable: {name}")
    return value

def default_pybind_includes():
    include_flags = []
    try:
        import sysconfig
        py_inc = sysconfig.get_paths().get("include")
        if py_inc:
            include_flags.append(f"-I{py_inc}")
    except Exception:
        pass
    try:
        import pybind11
        include_flags.append(f"-I{pybind11.get_include()}")
    except Exception:
        pass
    return " ".join(include_flags)

prefix = os.environ.get("CASCADE_PREFIX", os.path.expanduser("~/.local"))
site_packages = os.environ.get("CASCADE_PYPLUGIN_DIR", os.path.join(prefix, "lib", "cascade", "pyplugin"))
plugin_lib_dir = os.environ.get("CASCADE_PLUGIN_DIR", os.path.join(prefix, "lib", "cascade", "plugin"))
plugin_include_dir = os.environ.get("CASCADE_PLUGIN_INCLUDE_DIR", os.path.join(prefix, "include", "cascade", "plugin"))
core_inc = os.environ.get("CASCADE_CORE_INCLUDE", os.path.join(prefix, "include", "cascade"))
core_lib = os.environ.get("CASCADE_CORE_LIB", os.path.join(prefix, "lib"))
root_config = require_env("ROOT_CONFIG", "root-config")
pkg_config = require_env("PKG_CONFIG", "pkg-config")
pybind_includes_env = os.environ.get("PYBIND11_INCLUDES", "") or default_pybind_includes()
if not pybind_includes_env:
    raise RuntimeError("Missing required pybind11 include paths. Set PYBIND11_INCLUDES or install pybind11.")

class_map = {}
class_map_raw = os.environ.get("CASCADE_PLUGIN_CLASS_MAP", "")
if class_map_raw:
    try:
        class_map = json.loads(class_map_raw)
    except Exception as e:
        raise RuntimeError(f"Invalid CASCADE_PLUGIN_CLASS_MAP JSON: {e}")

env = Environment(ENV=os.environ)
pybind_includes = []
for token in pybind_includes_env.split():
    if token.startswith("-I"):
        pybind_includes.append(token[2:])
    else:
        pybind_includes.append(token)

env.ParseConfig(f'{root_config} --cflags --libs')
env.ParseConfig(f'{pkg_config} --cflags --libs yaml-cpp')
env.Append(CPPPATH=pybind_includes)
env.Append(CPPPATH=[core_inc, "include", "build/include"])
env.Append(LIBPATH=[core_lib])
env.Append(RPATH=[core_lib])
env.Append(LIBS=["Cascade"])
env.Append(CXXFLAGS=["-std=c++17", "-O2", "-fvisibility=default"])
env.Append(CXXFLAGS=["-DCASCADE_PLUGIN_NO_AUTO_REGISTER"])

install_targets = []
libs = []

# -------- C++ modules --------
headers = [str(h) for h in Glob("include/*.hh")]
modules = [os.path.splitext(os.path.basename(h))[0] for h in headers]

for mod in modules:
    cc_in = f"src/{mod}.cc"
    hh_in = f"include/{mod}.hh"
    version_hash = hash_tree([cc_in, hh_in])

    subs = {
        "@BASENAME@": mod,
        "@VERSION_HASH@": f"src:{version_hash}",
    }

    cc_out = f"build/src/{mod}.cc"
    hh_out = f"build/include/{mod}.hh"
    patch_file(cc_in, cc_out, subs)
    patch_file(hh_in, hh_out, subs)

    sources = [cc_out]

    # Auto-generate ABI entry if not provided in source
    cc_text = read_text(cc_in)
    if "CascadeRegisterPlugin" not in cc_text:
        class_name = class_map.get(mod, mod)
        entry_out = f"build/src/{mod}_plugin_entry.cc"
        entry_src = f"""#include "PluginABI.hh"
#include "{mod}.hh"

CASCADE_PLUGIN_EXPORT_ABI
CASCADE_PLUGIN_EXPORT void CascadeRegisterPlugin() {{ CASCADE_REGISTER_MODULE({class_name}); }}
"""
        write_text(entry_out, entry_src)
        sources.append(entry_out)

    lib = env.SharedLibrary(mod, sources)
    libs.append(lib)

    install_targets += env.Install(plugin_lib_dir, lib)
    install_targets += env.Install(plugin_include_dir, [hh_out])

# -------- Python modules --------
py_srcs = [str(p) for p in Glob("python/*.py")]
py_mods = [os.path.splitext(os.path.basename(p))[0] for p in py_srcs]

py_install = []
for mod, py_in in zip(py_mods, py_srcs):
    version_hash = hash_tree([py_in])
    subs = {
        "@BASENAME@": mod,
        "@VERSION_HASH@": f"src:{version_hash}",
    }
    py_out = f"build/python/{mod}.py"
    patch_file(py_in, py_out, subs)
    py_install += env.Install(site_packages, py_out)

# __init__.py (lazy imports)
def generate_init_py(target, source, env):
    files = [os.path.basename(str(f)) for f in source if str(f).endswith(".py")]
    lines = [
        "# Auto-generated cascade_plugins __init__.py",
        "import importlib",
        "",
        "_LAZY_MODULES = {",
    ]
    for f in sorted(files):
        if f == "__init__.py":
            continue
        name = f[:-3]
        lines.append(f"    \"{name}\": \"{name}\",")
    lines += [
        "}",
        "",
        "__all__ = list(_LAZY_MODULES.keys())",
        "",
        "def __getattr__(name):",
        "    if name in _LAZY_MODULES:",
        "        mod = importlib.import_module(f\".{_LAZY_MODULES[name]}\", __name__)",
        "        obj = getattr(mod, name)",
        "        globals()[name] = obj",
        "        return obj",
        "    raise AttributeError(f\"module {__name__!r} has no attribute {name!r}\")",
    ]
    Path(str(target[0])).write_text("\\n".join(lines) + "\\n")
    print(f"[SCons] generated __init__.py in {site_packages}")
    return 0

init_py_target = os.path.join(site_packages, "__init__.py")
init_py = env.Command(init_py_target, py_install, generate_init_py)
install_targets.append(init_py)

# -------- Alias --------
Default(libs)
env.Alias("install", install_targets)
