#!/usr/bin/env python3
"""Check an installed Cascade runtime against the source version headers."""

import argparse
from pathlib import Path
import re


def source_versions():
    include = Path(__file__).resolve().parents[1] / "include"
    version_header = (include / "Version.hh").read_text(encoding="utf-8")
    abi_header = (include / "PluginABI.hh").read_text(encoding="utf-8")

    def macro(header, name, pattern):
        match = re.search(rf"^#define {name} {pattern}$", header, re.MULTILINE)
        if match is None:
            raise ValueError(f"Missing or invalid {name} in version headers")
        return match.group(1)

    version = ".".join(
        macro(version_header, f"CASCADE_VERSION_{part}", r"(\d+)")
        for part in ("MAJOR", "MINOR", "PATCH")
    )
    version += macro(version_header, "CASCADE_VERSION_PRERELEASE", r'"([^"]*)"')
    abi = int(macro(abi_header, "CASCADE_PLUGIN_ABI_VERSION", r"(\d+)"))
    return version, abi


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag", help="Release tag, including the v prefix")
    args = parser.parse_args()
    version, abi = source_versions()
    if args.tag is not None and args.tag != f"v{version}":
        parser.error(f"Release tag {args.tag!r} does not match source version v{version}")

    import cascade

    actual = (cascade.__version__, cascade.__abi_version__)
    if actual != (version, abi):
        parser.error(f"Runtime version/ABI {actual!r} does not match source {(version, abi)!r}")
    if cascade.__abi_tag__.split(";", 1)[0] != f"abi={abi}":
        parser.error(f"Runtime ABI tag {cascade.__abi_tag__!r} does not match ABI {abi}")
    print(cascade.__version__, cascade.__abi_version__, cascade.__abi_tag__)


if __name__ == "__main__":
    main()
