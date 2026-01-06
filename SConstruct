import os, subprocess, hashlib, stat
from pathlib import Path
from SCons.Script import Environment, Variables
import SCons.Util

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
    lines = [
        "# Auto-generated cascade __init__.py\n",
        "from ._cascade import log_level, set_log_level, set_log_file, init_interrupt, is_interrupted, log, get_version, get_abi_version",
        "import importlib",
        "",
        "__version__ = get_version()",
        "__abi_version__ = get_abi_version()",
        "",
        "_LAZY_MODULES = {",
    ]
    for fname in sorted(files):
        modulename = fname[:-3]
        lines.append(f"    \"{modulename}\": \"{modulename}\",")
    lines += [
        "}",
        "",
        "__all__ = [",
        "    \"log_level\",",
        "    \"set_log_level\",",
        "    \"set_log_file\",",
        "    \"get_version\",",
        "    \"get_abi_version\",",
        "    \"__version__\",",
        "    \"__abi_version__\",",
        "    \"init_interrupt\",",
        "    \"is_interrupted\",",
        "    \"log\",",
        "] + list(_LAZY_MODULES.keys())",
        "",
        "def __getattr__(name):",
        "    if name in _LAZY_MODULES:",
        "        mod = importlib.import_module(f\".{_LAZY_MODULES[name]}\", __name__)",
        "        obj = getattr(mod, name)",
        "        globals()[name] = obj",
        "        return obj",
        "    raise AttributeError(f\"module {__name__!r} has no attribute {name!r}\")",
    ]
    with open(str(target[0]), "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"[SCons] __init__.py for cascade generated in {target_dir}")
    return 0

def make_executable(target, source, env):
    for t in target:
        path = str(t)
        st = os.stat(path)
        os.chmod(path, st.st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

def create_symlink(target, source, env):
    link_path = str(target[0])
    target_path = str(source[0])
    os.makedirs(os.path.dirname(link_path), exist_ok=True)
    if os.path.lexists(link_path):
        os.remove(link_path)
    rel_target = os.path.relpath(target_path, os.path.dirname(link_path))
    os.symlink(rel_target, link_path)
    print(f"[SCons] symlink {link_path} -> {rel_target}")
    return 0

include_dir = Path("modules/include")
module_headers = list(include_dir.glob("*Module.hh"))
modules = [f.stem for f in module_headers if f.suffix == ".hh"]

py_dir = Path("modules/python")
module_py = list(py_dir.glob("*.py"))
pymodules = [f.stem for f in module_py if f.suffix == ".py"]

for mod in modules:
    base_input = "modules/base/IAnalysisModule.hh"
    cc_input = f"modules/src/{mod}.cc"
    hh_input = f"modules/include/{mod}.hh"
    cc_output = f"build/patched/modules/src/{mod}.cc"
    hh_output = f"build/patched/modules/include/{mod}.hh"
    base_hash = hash_file(base_input)
    cc_hash = hash_file(cc_input)
    hh_hash = hash_file(hh_input)
    combined_hash = (base_hash + cc_hash + hh_hash)

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
vars.Add('PREFIX', 'install directory', '~/.local')
vars.Add('LIBDIR', 'library install directory', '')
vars.Add('BINDIR', 'binary install directory', '')
vars.Add('INCLUDEDIR', 'header install directory', '')
vars.Add('PYTHONDIR', 'python package install directory', '')
vars.Add('PYMODULEDIR', 'python module install directory (cascade.pymodule)', '')

env = Environment(ENV=os.environ, variables=vars)
env.Append()
prefix = os.path.expanduser(env['PREFIX'])
env['PREFIX'] = prefix
if not env['LIBDIR']:
    env['LIBDIR'] = os.path.join(env['PREFIX'], 'lib')
if not env['BINDIR']:
    env['BINDIR'] = os.path.join(env['PREFIX'], 'bin')
if not env['INCLUDEDIR']:
    env['INCLUDEDIR'] = os.path.join(env['PREFIX'], 'include', 'cascade')
if not env['PYTHONDIR']:
    env['PYTHONDIR'] = os.path.join(env['LIBDIR'], 'cascade')
if not env['PYMODULEDIR']:
    env['PYMODULEDIR'] = os.path.join(env['PYTHONDIR'], 'pymodule')

pybind_flags = os.popen("python3 -m pybind11 --includes").read().strip().split()
pybind_includes = [flag[2:] for flag in pybind_flags if flag.startswith("-I")]

env.ParseConfig('root-config --cflags --libs')
env.ParseConfig('pkg-config --cflags --libs yaml-cpp')
env.Append(CXXFLAGS=["-std=c++17","-O2","-fvisibility=default"])
env.Append(CPPPATH=pybind_includes)
env.Append(LIBS=["ssl","crypto"])
env.Append(RPATH=[env['LIBDIR'], '$ORIGIN', '$ORIGIN/..'])
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





env.Tool('compilation_db')
compdb = env.CompilationDatabase('compile_commands.json')
AlwaysBuild(compdb)



def tidy_action(target, source, env):
    import subprocess, shlex, os
    build_dir = os.path.abspath('build')
    tidy = env.WhereIs('clang-tidy') or 'clang-tidy'
    base = f"{tidy} -p {build_dir} --quiet"
    if os.path.exists('.clang-tidy'):
        base += " --config-file=.clang-tidy"
    failed = []
    for s in source:
        s = str(s)
        if s.endswith(('.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp')):
            cmd = f"{base} {shlex.quote(s)}"
            print(f"[clang-tidy] {cmd}")
            if subprocess.call(cmd, shell=True) != 0:
                failed.append(s)
    return 0 if not failed else 1

Tidy = Builder(action=Action(tidy_action, cmdstr='[TIDY] $SOURCE'))
env.Append(BUILDERS={'Tidy': Tidy})

all_srcs = []
all_srcs += Glob('src/*.cc') + Glob('src/*.hh')
all_srcs += Glob('AnalysisManager/*.cc') + Glob('AnalysisManager/*.hh')
all_srcs += Glob('PlotManager/*.cc') + Glob('PlotManager/*.hh')
all_srcs += Glob('ParamManager/*.cc') + Glob('ParamManager/*.hh')
all_srcs += Glob('modules/src/*.cc') + Glob('modules/include/*.hh')

env.Alias('tidy', env.Tidy('tidy.log', all_srcs))
Depends('tidy', compdb)
Depends('tidy', '.clang-tidy')



# SConscripts
utils_obj, utils_install = SConscript("build/utils/SConscript", exports=["env", "TOP"])
lib_analysis_obj, lib_analysis_install = SConscript("build/AnalysisManager/SConscript", exports=["env","TOP"])
lib_plot_obj, lib_plot_install = SConscript("build/PlotManager/SConscript", exports=["env","TOP"])
lib_param_obj, lib_param_install = SConscript("build/ParamManager/SConscript", exports=["env","TOP"])
core_objs, _ = SConscript("build/src/SConscript", exports=["env" , "lib_param_obj"])
module_libs_obj, module_libs_install = SConscript("build/modules/SConscript", exports=["env", "lib_param_obj"])
pybind_obj, pybind_install = SConscript("build/main/SConscript", exports=[
    "env", "core_objs", "utils_obj", "lib_param_obj" ,"module_libs_obj", "lib_analysis_obj", "lib_plot_obj"
])

# pyinstall
cascade_dir = env['PYTHONDIR']
cascade_files = Glob("python/*.py")
py_install = env.Install(cascade_dir, cascade_files)

cascade_cli_dir = env['BINDIR']
cascade_cli = Glob("python/cascade")
cli_install = env.Install(cascade_cli_dir, cascade_cli)
env.AddPostAction(cli_install,make_executable)

scripts_dir = os.path.join(env['PREFIX'], "share", "cascade", "scripts")
sign_script = env.Install(scripts_dir, "scripts/sign_plugin.sh")
env.AddPostAction(sign_script, make_executable)

cascade_init_target = os.path.join(cascade_dir, "__init__.py")
cascade_init = env.Command(cascade_init_target, py_install, generate_init_py_head)

cascade_so_target = os.path.join(cascade_dir, f"_cascade{env['SHLIBSUFFIX']}")
cascade_so_link = env.Command(cascade_so_target, pybind_install, create_symlink)
Depends(cascade_so_link, pybind_install)

pymodule_dir = env['PYMODULEDIR']
pymodule_files = Glob("build/modules/python/*.py")
pymodule_install = env.Install(pymodule_dir, pymodule_files)
py_install += pymodule_install

pymodule_init_target = os.path.join(pymodule_dir, "__init__.py")
pymodule_init = env.Command(pymodule_init_target, pymodule_install, generate_init_py)


hdr_install = []

if os.path.isdir('build/modules/include'):
    hdr_install += env.Install(os.path.join(env['INCLUDEDIR']), Glob('build/modules/include/*.hh'))

if os.path.isdir('modules'):
    hdr_install += env.Install(os.path.join(env['INCLUDEDIR']), Glob('modules/base/*.hh'))
if os.path.isdir('include'):
    hdr_install += env.Install(env['INCLUDEDIR'], Glob('include/*.hh'))

for sub in ['AnalysisManager', 'PlotManager', 'ParamManager', 'utils', 'src']:
    if os.path.isdir(sub):
        globs = Glob(f'{sub}/*.hh')
        if globs:
            hdr_install += env.Install(os.path.join(env['INCLUDEDIR']), globs)

# cppinstall
install_targets = lib_analysis_install + utils_install + lib_param_install + lib_plot_install + module_libs_install + pybind_install + py_install + cascade_init + cascade_so_link + pymodule_init + cli_install + hdr_install + sign_script
build_targets = utils_obj + lib_analysis_obj + lib_param_obj + lib_plot_obj + module_libs_obj + pybind_obj

install_targets = SCons.Util.unique(install_targets)
build_targets = SCons.Util.unique(build_targets)

Depends(compdb, build_targets)
env.Alias('compdb', compdb)


env.Alias("install", install_targets)
Default(build_targets + install_targets)
