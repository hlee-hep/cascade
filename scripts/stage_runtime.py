#!/usr/bin/env python3
"""Install Cascade into an empty staging tree for packaging or deployment.

The result contains Cascade only; Python, ROOT, and system dependencies must be
provided by the matching runtime environment. Existing destinations are refused.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def stage_runtime(output, *, python=sys.executable, root_config="root-config", jobs=2):
    output = Path(output).absolute()
    if output.exists() or output.is_symlink():
        raise FileExistsError(f"Staging destination already exists: {output}")
    if jobs < 1:
        raise ValueError("jobs must be positive")
    output.parent.mkdir(parents=True, exist_ok=True)
    # Keep the temporary directory on the destination filesystem for promotion.
    with tempfile.TemporaryDirectory(prefix=".cascade-stage-", dir=output.parent) as temporary:
        prefix = Path(temporary) / "runtime"
        subprocess.run(
            [python, "-m", "SCons", "install", f"-j{jobs}", f"PREFIX={prefix}",
             f"PYTHON={python}", f"ROOT_CONFIG={root_config}"],
            cwd=Path(__file__).resolve().parents[1], check=True,
        )
        if output.exists() or output.is_symlink():
            raise FileExistsError(f"Staging destination appeared during installation: {output}")
        os.rename(prefix, output)
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True)
    parser.add_argument("--python", default=sys.executable)
    parser.add_argument("--root-config", default=shutil.which("root-config") or "root-config")
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    print(stage_runtime(args.output, python=args.python, root_config=args.root_config, jobs=args.jobs))


if __name__ == "__main__":
    main()
