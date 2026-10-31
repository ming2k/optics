#!/usr/bin/env python3
"""Run clang-tidy over shipped library translation units.

Scope: libs/**/src/*.c. Platform-specific backends (Win32, Cocoa) are cross-checked
separately in dedicated workflows. Zero-warning baseline policy.
"""

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

EXCLUDE_SUFFIXES = ("_win32.c", "_directwrite.c", "_cocoa.c", "_coretext.c")


def get_tidy_sources(repo_root: Path) -> list[str]:
    sources = []
    libs_dir = repo_root / "libs"
    for root, dirs, files in os.walk(libs_dir):
        if "subprojects" in dirs:
            dirs.remove("subprojects")
        if "vendor" in dirs:
            dirs.remove("vendor")
        if os.path.basename(root) != "src" and "src" not in root.split(os.sep):
            continue
        for f in sorted(files):
            if f.endswith(".c") and not f.endswith(EXCLUDE_SUFFIXES):
                sources.append(os.path.join(root, f))
    sources.sort()
    return sources


def run_tidy(repo_root: Path, build_dir: str = "build-tidy", clang_tidy: str = "clang-tidy") -> tuple[bool, str, str]:
    b_path = repo_root / build_dir
    if not b_path.is_dir():
        setup_cmd = [
            "meson", "setup", build_dir,
            "-Dtests=true", "-Dexamples=false", "--buildtype=debug",
            "-Dfreetype2:harfbuzz=disabled", "-Dharfbuzz:cairo=disabled",
        ]
        env = os.environ.copy()
        env["CC"] = "clang"
        res = subprocess.run(setup_cmd, cwd=repo_root, env=env, capture_output=True, text=True)
        if res.returncode != 0:
            return False, "", f"error: meson setup failed:\n{res.stderr}"

    # Build once to ensure generated headers exist
    compile_res = subprocess.run(["meson", "compile", "-C", build_dir], cwd=repo_root, capture_output=True, text=True)
    if compile_res.returncode != 0:
        return False, "", f"error: build failed in {build_dir}:\n{compile_res.stderr}"

    sources = get_tidy_sources(repo_root)
    if not sources:
        return False, "", "error: no source files found for clang-tidy"

    failed = False
    output_lines = []

    for src in sources:
        rel = os.path.relpath(src, repo_root)
        cmd = [clang_tidy, "-p", str(b_path), src]
        res = subprocess.run(cmd, cwd=repo_root, capture_output=True, text=True)
        combined = res.stdout + "\n" + res.stderr
        if re.search(r"warning:|error:", combined):
            failed = True
            output_lines.append(f"=== {rel}")
            for line in combined.splitlines():
                if "warning:" in line or "error:" in line:
                    output_lines.append(line)

    if failed:
        output_lines.append("\nclang-tidy found issues above. Fix them; do not silence without a reason.")
        return False, "", "\n".join(output_lines)

    return True, f"clang-tidy clean ({build_dir})", ""


def main() -> int:
    parser = argparse.ArgumentParser(description="Run clang-tidy on shipped library translation units.")
    parser.add_argument("build_dir", nargs="?", default="build-tidy", help="Build directory with compile_commands.json")
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parent.parent.parent
    ok, stdout_msg, stderr_msg = run_tidy(repo_root, build_dir=args.build_dir)
    if ok:
        print(stdout_msg)
        return 0
    sys.stderr.write(stderr_msg + "\n")
    return 1


if __name__ == "__main__":
    sys.exit(main())
