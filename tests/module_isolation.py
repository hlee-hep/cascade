import sys


def snapshot_package_modules(package):
    prefix = package + "."
    return {
        name: module
        for name, module in sys.modules.items()
        if name == package or name.startswith(prefix)
    }


def restore_package_modules(package, snapshot):
    prefix = package + "."
    for name in list(sys.modules):
        if name == package or name.startswith(prefix):
            sys.modules.pop(name, None)
    sys.modules.update(snapshot)
