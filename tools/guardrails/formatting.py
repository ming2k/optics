#!/usr/bin/env python3
"""Format guardrail: asserts every C source is byte-identical to pinned clang-format (22.x).

Vendored code and build trees are excluded.
Supports --fix to format files in place.
"""

import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

PINNED_MAJOR = 22
EXCLUDE_DIRS = {".git", "build", "build-release", "build-tidy", "subprojects", "vendor"}


def get_c_sources(repo_root: Path) -> list[str]:
    sources = []
    scan_roots = [repo_root / "libs", repo_root / "examples", repo_root / "tests"]
    for base in scan_roots:
        if not base.is_dir():
            continue
        for root, dirs, files in os.walk(base):
            dirs[:] = [d for d in dirs if d not in EXCLUDE_DIRS and not d.startswith(".")]
            for f in files:
                if f.endswith((".c", ".h")):
                    full = os.path.join(root, f)
                    if "vendor" not in full:
                        sources.append(full)
    sources.sort()
    return sources


def check_clang_format_version(cf_bin: str) -> tuple[bool, str]:
    try:
        res = subprocess.run([cf_bin, "--version"], capture_output=True, text=True, check=True)
        m = re.search(r"([0-9]+)\.([0-9]+)\.([0-9]+)", res.stdout)
        if not m:
            return False, f"Cannot parse version from: {res.stdout.strip()}"
        major = int(m.group(1))
        if major != PINNED_MAJOR:
            return (
                False,
                f"error: clang-format {PINNED_MAJOR}.x is pinned; found {m.group(0)}\n"
                f"       install: apt-get install clang-format-{PINNED_MAJOR} (apt.llvm.org)",
            )
        return True, ""
    except Exception as e:
        return False, f"error: clang-format not found or failed to execute: {e}"


def run_format_check(repo_root: Path, fix: bool = False, cf_bin: str = "clang-format") -> tuple[bool, str, str]:
    ok, ver_err = check_clang_format_version(cf_bin)
    if not ok:
        return False, "", ver_err

    sources = get_c_sources(repo_root)
    if not sources:
        return False, "", "error: no C sources found"

    if fix:
        res = subprocess.run([cf_bin, "-i"] + sources, cwd=repo_root, capture_output=True, text=True)
        if res.returncode == 0:
            return True, f"reformatted in place: {len(sources)} files", ""
        return False, "", res.stderr

    # Check mode with --dry-run --Werror
    res = subprocess.run([cf_bin, "--dry-run", "--Werror"] + sources, cwd=repo_root, capture_output=True, text=True)
    if res.returncode == 0:
        return True, "code formatting clean", ""

    # Extract files needing format from stderr/stdout
    combined = res.stderr + "\n" + res.stdout
    bad_files = set()
    for line in combined.splitlines():
        m = re.match(r"^([^:]+\.[ch]):[0-9]+", line)
        if m:
            bad_files.add(m.group(1))

    err_lines = ["code formatting check failed; the following files need formatting:"]
    for f in sorted(bad_files):
        err_lines.append(f"  {f}")
    err_lines.append("run './tools/optics fix' to reformat in place.")
    return False, "", "\n".join(err_lines)


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent.parent
    fix = "--fix" in sys.argv
    ok, stdout_msg, stderr_msg = run_format_check(repo_root, fix=fix)
    if ok:
        if stdout_msg:
            print(stdout_msg)
        return 0
    if stderr_msg:
        sys.stderr.write(stderr_msg + "\n")
    return 1


if __name__ == "__main__":
    sys.exit(main())
