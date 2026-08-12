#!/usr/bin/env python3

import argparse
import json
import os
import pathlib
import platform
import subprocess
from datetime import datetime, timezone


def command_output(command):
    try:
        return subprocess.check_output(command, text=True, stderr=subprocess.STDOUT).strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def cpu_model():
    cpuinfo = pathlib.Path("/proc/cpuinfo")
    if cpuinfo.is_file():
        for line in cpuinfo.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.lower().startswith("model name") and ":" in line:
                return line.split(":", 1)[1].strip()
    return platform.processor() or "unknown"


def main():
    parser = argparse.ArgumentParser(description="Record Cascade benchmark environment metadata")
    parser.add_argument("--output", required=True)
    parser.add_argument("--dag", required=True)
    parser.add_argument("--input-hash", required=True)
    parser.add_argument("--output-hash", required=True)
    args = parser.parse_args()

    results = {
        "dag": pathlib.Path(args.dag),
        "input_hash": pathlib.Path(args.input_hash),
        "output_hash": pathlib.Path(args.output_hash),
    }
    for label, path in results.items():
        with path.open(encoding="utf-8") as stream:
            json.load(stream)

    metadata = {
        "schema": "cascade.benchmark-baseline",
        "schema_version": 1,
        "captured_at": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
        "commit": os.environ.get("GITHUB_SHA") or command_output(["git", "rev-parse", "HEAD"]),
        "runner": {
            "os": os.environ.get("RUNNER_OS", platform.system()),
            "architecture": platform.machine(),
            "cpu": cpu_model(),
            "logical_cpus": os.cpu_count(),
            "kernel": platform.release(),
        },
        "toolchain": {
            "cascade": command_output(["git", "describe", "--tags", "--always", "--dirty"]),
            "root": command_output(["root-config", "--version"]),
            "compiler": command_output(["c++", "--version"]).splitlines()[0],
            "python": platform.python_version(),
        },
        "results": {label: path.name for label, path in results.items()},
    }
    output = pathlib.Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
