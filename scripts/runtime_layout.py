"""Keep an owned generated runtime tree aligned with its current build outputs."""
from pathlib import Path


def prune_runtime(root, expected):
    root = Path(root).absolute()
    if root.is_symlink():
        raise ValueError(f"Runtime root must not be a symlink: {root}")
    files = {Path(path).absolute().relative_to(root) for path in expected}
    directories = {parent for path in files for parent in path.parents}

    def visit(directory):
        for path in directory.iterdir():
            relative = path.relative_to(root)
            if path.is_symlink() or not path.is_dir():
                if relative not in files:
                    path.unlink()
            else:
                visit(path)
                if relative not in directories:
                    path.rmdir()

    if root.exists():
        visit(root)
