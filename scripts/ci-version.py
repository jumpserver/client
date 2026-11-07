#!/usr/bin/env python3
"""Resolve an installer-safe version without evaluating workflow inputs as code."""
import argparse
import json
import os
from pathlib import Path
import re


def version(value):
    match = re.fullmatch(r"v?(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", value)
    if not match:
        raise ValueError("Version must be X.Y.Z (or a vX.Y.Z tag), without a prerelease suffix")
    parts = tuple(map(int, match.groups()))
    if parts[0] > 255 or parts[1] > 255 or parts[2] > 65535:
        raise ValueError("Windows MSI requires major/minor <= 255 and patch <= 65535")
    return ".".join(map(str, parts))


def resolve(environ, cmake):
    tag = environ.get("GITHUB_REF_TYPE") == "tag"
    requested = environ.get("REQUESTED_VERSION", "").strip()
    ref_version = version(environ.get("GITHUB_REF_NAME", "")) if tag else None
    if requested:
        selected = version(requested)
        if tag and selected != ref_version:
            raise ValueError("Requested version does not match the tag")
    elif tag:
        selected = ref_version
    else:
        match = re.search(r'set\(CLIENT_VERSION\s+"([0-9.]+)"', cmake)
        if not match:
            raise ValueError("CMakeLists.txt has no numeric CLIENT_VERSION default")
        selected = version(match.group(1))
    windows = [{"arch": "amd64", "qt_arch": "x86_64", "msvc_arch": "amd64"}]
    if environ.get("INCLUDE_WINDOWS_X86", "false") == "true":
        windows.append({"arch": "386", "qt_arch": "x86", "msvc_arch": "amd64_x86"})
    release_tag = environ.get("GITHUB_REF_NAME", "") if tag else "v" + selected
    return {
        "version": selected,
        "release_tag": release_tag,
        "sign_macos": str(tag or environ.get("SIGN_MACOS") == "true").lower(),
        "sign_windows": str(environ.get("SIGN_WINDOWS") == "true").lower(),
        "windows_matrix": json.dumps({"include": windows}, separators=(",", ":")),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cmake", type=Path, default=Path(__file__).resolve().parents[1] / "CMakeLists.txt")
    parser.add_argument("--github-output", type=Path)
    args = parser.parse_args()
    try:
        values = resolve(os.environ, args.cmake.read_text(encoding="utf-8"))
    except ValueError as error:
        parser.error(str(error))
    lines = "".join(f"{key}={value}\n" for key, value in values.items())
    if args.github_output:
        with args.github_output.open("a", encoding="utf-8") as output:
            output.write(lines)
    print(lines, end="")


if __name__ == "__main__":
    main()
