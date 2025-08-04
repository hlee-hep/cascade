import os, subprocess, hashlib
from pathlib import Path
from SCons.Script import Environment, Variables

def hash_file(path):
    if not os.path.exists(path):
        return ""
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()[:12]

def patch_file(input_path, output_path, substitutions):
    with open(input_path, "r") as f:
        content = f.read()
    for key, val in substitutions.items():
        content = content.replace(key, val)
    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    with open(output_path, "w") as f:
        f.write(content)

def generate_init_py(target, source, env):
    target_dir = os.path.dirname(str(target[0]))
    files = [f for f in os.listdir(target_dir) if f.endswith(".py") and f != "__init__.py"]
    lines = ["# Auto-generated cascade __init__.py\n"]
    for fname in sorted(files):
        modulename = fname[:-3]
        lines.append(f"from .{modulename} import {modulename}")
    with open(str(target[0]), "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"[SCons] __init__.py for cascade generated in {target_dir}")
    return 0

def generate_init_py_head(target, source, env):
    target_dir = os.path.dirname(str(target[0]))
    files = [f for f in os.listdir(target_dir) if f.endswith(".py") and f != "__init__.py"]
    lines = ["# Auto-generated cascade __init__.py\n"]
    for fname in sorted(files):
        modulename = fname[:-3]
        lines.append(f"from ._cascade import AMCM, log_level, set_log_level, set_log_file")
        lines.append(f"from .{modulename} import {modulename}")
    with open(str(target[0]), "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"[SCons] __init__.py for cascade generated in {target_dir}")
    return 0

include_dir = Path("modules/include")
module_headers = list(include_dir.glob("*Module.hh"))
modules = [f.stem for f in module_headers if f.suffix == ".hh"]

py_dir = Path("modules/python")
module_py = list(py_dir.glob("*.py"))
pymodules = [f.stem for f in module_py if f.suffix == ".py"]

for mod in modules:
    cc_input = f"modules/src/{mod}.cc"
    hh_input = f"modules/include/{mod}.hh"
    cc_output = f"build/modules/src/{mod}.cc"
    hh_output = f"build/modules/include/{mod}.hh"
    cc_hash = hash_file(cc_input)
    hh_hash = hash_file(hh_input)
    combined_hash = (cc_hash + hh_hash)

    substitutions = {
        "@BASENAME@": f"{mod}",
        "@VERSION_HASH@": f"src:{combined_hash}"
    }

    if os.path.exists(cc_input):
        patch_file(cc_input, cc_output, substitutions)
    if os.path.exists(hh_input):
        patch_file(hh_input, hh_output, substitutions)

for mod in pymodules:
    py_input = f"modules/python/{mod}.py"
    py_output = f"build/modules/python/{mod}.py"
    py_hash = hash_file(py_input)
    combined_hash = (py_hash + py_hash)
    print(py_input)
    substitutions = {
        "@BASENAME@": f"{mod}",
        "@VERSION_HASH@": f"src:{combined_hash}"
    }

    if os.path.exists(py_input):
        patch_file(py_input, py_output, substitutions)

vars = Variables()
vars.Add('PREFIX', 'install directory', '/home/hobinlee/.local')

env = Environment(ENV=os.environ, variables=vars)
env.Append()
prefix = os.path.expanduser(env['PREFIX'])
env['PREFIX'] = prefix

env['LIBDIR'] = os.path.join(env['PREFIX'], 'lib')

pybind_flags = os.popen("python3 -m pybind11 --includes").read().strip().split()
pybind_includes = [flag[2:] for flag in pybind_flags if flag.startswith("-I")]

# YAML
yaml_flags = os.popen("pkg-config --cflags yaml-cpp").read().strip().split()
yaml_libs = os.popen("pkg-config --libs yaml-cpp").read().strip().split()

# ROOT
root_cflags = os.popen("root-config --cflags").read().strip().split()
root_libs = os.popen("root-config --libs").read().strip().split()

env.Append(CXXFLAGS=["-std=c++17","-O2","-fvisibility=default"] + root_cflags + yaml_flags)
env.Append(CPPPATH=pybind_includes)
env.Append(LINKFLAGS=root_libs)
env.Append(LIBS=yaml_libs)
env.Append(LIBS=["ssl","crypto"])
VariantDir("build/src", "src", duplicate=0)
VariantDir("build/utils", "utils", duplicate=0)
VariantDir("build/AnalysisManager", "AnalysisManager", duplicate=0)
VariantDir("build/PlotManager", "PlotManager", duplicate=0)
VariantDir("build/ParamManager", "ParamManager", duplicate=0)
VariantDir("build/modules", "modules", duplicate=0)
VariantDir("build/python", "python", duplicate=0)
VariantDir("build/main", "main", duplicate=0)
TOP = os.getcwd()  

env.Append(CPPPATH=[
    os.path.join(TOP, "build/modules/include"),
    os.path.join(TOP, "modules/base"),
    os.path.join(TOP, "src"),
    os.path.join(TOP, "AnalysisManager"),
    os.path.join(TOP, "PlotManager"),
    os.path.join(TOP, "ParamManager"),
    os.path.join(TOP, "include"),
    os.path.join(TOP, "utils"),
])



# SConscripts
utils_obj, utils_install = SConscript("build/utils/SConscript", exports=["env", "TOP"])
lib_analysis_obj, lib_analysis_install = SConscript("build/AnalysisManager/SConscript", exports=["env","TOP"])
lib_plot_obj, lib_plot_install = SConscript("build/PlotManager/SConscript", exports=["env","TOP"])
lib_param_obj, lib_param_install = SConscript("build/ParamManager/SConscript", exports=["env"])
core_objs, _ = SConscript("build/src/SConscript", exports=["env" , "lib_param_obj"])
module_libs_obj, module_libs_install = SConscript("build/modules/SConscript", exports=["env", "lib_param_obj"])
pybind_obj, pybind_install = SConscript("build/main/SConscript", exports=[
    "env", "core_objs", "utils_obj", "lib_param_obj" ,"module_libs_obj", "lib_analysis_obj", "lib_plot_obj"
])

# pyinstall
cascade_dir = os.path.join(env['LIBDIR'], "cascade")
cascade_files = Glob("python/*.py")
py_install = env.Install(cascade_dir, cascade_files)

cascade_init_target = os.path.join(cascade_dir, "__init__.py")
cascade_init = env.Command(cascade_init_target, py_install, generate_init_py_head)

pymodule_dir = os.path.join(cascade_dir, "pymodule")
pymodule_files = Glob("build/modules/python/*.py")
pymodule_install = env.Install(pymodule_dir, pymodule_files)
py_install += pymodule_install

pymodule_init_target = os.path.join(pymodule_dir, "__init__.py")
pymodule_init = env.Command(pymodule_init_target, pymodule_install, generate_init_py)

# cppinstall
install_targets = lib_analysis_install + utils_install + lib_param_install + lib_plot_install + module_libs_install + pybind_install +py_install+cascade_init+pymodule_init
build_targets = utils_obj + lib_analysis_obj + lib_param_obj + lib_plot_obj + module_libs_obj + pybind_obj

env.Alias("install", install_targets)
Default(build_targets + install_targets)
