import os, subprocess, stat, sys
from SCons.Script import Environment, Variables
import SCons.Util

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
        "from ._cascade import log_level, set_log_level, set_log_file, init_interrupt, is_interrupted, log, get_version, get_abi_version, get_abi_tag",
        "import importlib",
        "",
        "__version__ = get_version()",
        "__abi_version__ = get_abi_version()",
        "__abi_tag__ = get_abi_tag()",
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
        "    \"get_abi_tag\",",
        "    \"__version__\",",
        "    \"__abi_version__\",",
        "    \"__abi_tag__\",",
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

def run_tests(target, source, env):
    test_binary = os.path.abspath(str(source[0]))
    build_library_paths = [
        os.path.abspath("build/src"),
        os.path.abspath("build/utils"),
        os.path.abspath("build/AnalysisManager"),
        os.path.abspath("build/ParamManager"),
        os.path.abspath("build/PlotManager"),
        os.path.abspath("build/main"),
    ]
    test_environment = {
        **os.environ,
        "CASCADE_CACHE_DIR": os.path.abspath("build/test-cache"),
        "TMPDIR": "/tmp",
        "LD_LIBRARY_PATH": os.pathsep.join(build_library_paths + [os.environ.get("LD_LIBRARY_PATH", "")]),
    }
    subprocess.check_call([test_binary], env=test_environment)
    for python_source in (
        "python/py_amcm.py",
        "python/plt_plot_manager.py",
        "python/cascade",
        "modules/python/base_module.py",
        "scripts/plugin_sconstruct",
    ):
        with open(python_source, "r", encoding="utf-8") as source_file:
            compile(source_file.read(), python_source, "exec")
    for python_source in Glob("python/cascade_cli/*.py"):
        source_path = str(python_source)
        with open(source_path, "r", encoding="utf-8") as source_file:
            compile(source_file.read(), source_path, "exec")
    subprocess.check_call(
        [sys.executable, "-m", "unittest", "discover", "-s", "tests", "-p", "test_*.py"],
        env=test_environment,
    )
    os.makedirs(os.path.dirname(str(target[0])), exist_ok=True)
    with open(str(target[0]), "w") as stamp:
        stamp.write("ok\n")
    return 0

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
env.AppendUnique(CXXFLAGS=["-std=c++17", "-O2", "-fvisibility=default"])
env.Append(CPPPATH=pybind_includes)
env.Append(LIBS=["ssl","crypto"])
env.Append(RPATH=[env['LIBDIR']])
VariantDir("build/src", "src", duplicate=0)
VariantDir("build/utils", "utils", duplicate=0)
VariantDir("build/AnalysisManager", "AnalysisManager", duplicate=0)
VariantDir("build/PlotManager", "PlotManager", duplicate=0)
VariantDir("build/ParamManager", "ParamManager", duplicate=0)
VariantDir("build/python", "python", duplicate=0)
VariantDir("build/main", "main", duplicate=0)
TOP = os.getcwd()

env.Append(CPPPATH=[
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
all_srcs += Glob('modules/base/IAnalysisModule.hh')

env.Alias('tidy', env.Tidy('tidy.log', all_srcs))
Depends('tidy', compdb)
Depends('tidy', '.clang-tidy')



# SConscripts
utils_obj, utils_install = SConscript("build/utils/SConscript", exports=["env", "TOP"])
lib_analysis_obj, lib_analysis_install = SConscript("build/AnalysisManager/SConscript", exports=["env","TOP"])
lib_plot_obj, lib_plot_install = SConscript("build/PlotManager/SConscript", exports=["env","TOP"])
lib_param_obj, lib_param_install = SConscript("build/ParamManager/SConscript", exports=["env","TOP"])
core_objs, core_install = SConscript("build/src/SConscript", exports=["env" , "lib_param_obj"])
pybind_obj, pybind_install = SConscript("build/main/SConscript", exports=[
    "env", "core_objs", "utils_obj", "lib_param_obj", "lib_analysis_obj", "lib_plot_obj"
])

# pyinstall
cascade_dir = env['PYTHONDIR']
cascade_files = Glob("python/*.py")
py_install = env.Install(cascade_dir, cascade_files)

cascade_cli_package_dir = os.path.join(os.path.dirname(cascade_dir), "cascade_cli")
cascade_cli_package = env.Install(cascade_cli_package_dir, Glob("python/cascade_cli/*.py"))

cascade_cli_dir = env['BINDIR']
cascade_cli = Glob("python/cascade")
cli_install = env.Install(cascade_cli_dir, cascade_cli)
env.AddPostAction(cli_install,make_executable)

scripts_dir = os.path.join(env['PREFIX'], "share", "cascade", "scripts")
sign_script = env.Install(scripts_dir, "scripts/sign_plugin.sh")
env.AddPostAction(sign_script, make_executable)
plugin_sconstruct = env.Install(scripts_dir, "scripts/plugin_sconstruct")

cascade_init_target = os.path.join(cascade_dir, "__init__.py")
cascade_init = env.Command(cascade_init_target, py_install + cascade_cli_package, generate_init_py_head)

cascade_so_target = os.path.join(cascade_dir, f"_cascade{env['SHLIBSUFFIX']}")
cascade_so_link = env.Command(cascade_so_target, pybind_install, create_symlink)
Depends(cascade_so_link, pybind_install)

pymodule_dir = env['PYMODULEDIR']
pymodule_files = Glob("modules/python/base_module.py")
pymodule_install = env.Install(pymodule_dir, pymodule_files)
py_install += pymodule_install

pymodule_init_target = os.path.join(pymodule_dir, "__init__.py")
pymodule_init = env.Command(pymodule_init_target, pymodule_install, generate_init_py)


hdr_install = []

if os.path.isdir('modules'):
    hdr_install += env.Install(os.path.join(env['INCLUDEDIR']), Glob('modules/base/IAnalysisModule.hh'))
if os.path.isdir('include'):
    hdr_install += env.Install(env['INCLUDEDIR'], Glob('include/*.hh'))

for sub in ['AnalysisManager', 'PlotManager', 'ParamManager', 'utils', 'src']:
    if os.path.isdir(sub):
        globs = Glob(f'{sub}/*.hh')
        if globs:
            hdr_install += env.Install(os.path.join(env['INCLUDEDIR']), globs)

# cppinstall
install_targets = core_install + lib_analysis_install + utils_install + lib_param_install + lib_plot_install + pybind_install + py_install + cascade_cli_package + cascade_init + cascade_so_link + pymodule_init + cli_install + hdr_install + sign_script + plugin_sconstruct
build_targets = utils_obj + lib_analysis_obj + lib_param_obj + lib_plot_obj + pybind_obj

install_targets = SCons.Util.unique(install_targets)
build_targets = SCons.Util.unique(build_targets)

Depends(compdb, build_targets)
env.Alias('compdb', compdb)


env.Alias("install", install_targets)

test_env = env.Clone()
test_rpath = [
    os.path.join(TOP, "build", "src"),
    os.path.join(TOP, "build", "utils"),
    os.path.join(TOP, "build", "AnalysisManager"),
    os.path.join(TOP, "build", "ParamManager"),
    os.path.join(TOP, "build", "PlotManager"),
]
test_env.Replace(RPATH=test_rpath)
test_env.Append(
    LIBPATH=[
        os.path.join(TOP, "build", "src"),
        os.path.join(TOP, "build", "utils"),
        os.path.join(TOP, "build", "AnalysisManager"),
        os.path.join(TOP, "build", "ParamManager"),
        os.path.join(TOP, "build", "PlotManager"),
    ],
    LIBS=["AMCM", "AnalysisManager", "ParamManager", "PlotManager", "utils"],
)
core_test_object = test_env.Object("build/tests/test_core.o", "tests/test_core.cc")
core_test = test_env.Program("build/tests/test_core", [core_test_object])
Depends(core_test, build_targets)
test_stamp = env.Command("build/tests/.passed", core_test, run_tests)
AlwaysBuild(test_stamp)
env.Alias("test", test_stamp)

Default(build_targets)
