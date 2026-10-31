#!/usr/bin/env python3
"""Material-boundary guardrail (ADR-0063).

Assures Layer 0 (flux) owns only rendering mechanisms and never references
named materials (liquid_glass, prism_). Materials live exclusively in prism.
Syntax-aware: strips C/GLSL and Meson comments to prevent false positives on documentation.
"""

import os
import re
import sys
from pathlib import Path

FORBIDDEN_RE = re.compile(r"liquid_glass|LIQUID_GLASS|prism_|PRISM_")
C_COMMENT_RE = re.compile(r"//[^\n]*|/\*.*?\*/", re.DOTALL)
MESON_COMMENT_RE = re.compile(r"#[^\n]*")

TARGET_EXTS = {".c", ".h", ".comp", ".vert", ".frag"}
EXCLUDE_DIRS = {".git", "build", "build-release", "build-tidy", "subprojects", "vendor"}


def strip_c_comments(source: str) -> str:
    def replacer(m):
        return "\n" * m.group(0).count("\n")
    return C_COMMENT_RE.sub(replacer, source)


def strip_meson_comments(source: str) -> str:
    def replacer(m):
        return "\n" * m.group(0).count("\n")
    return MESON_COMMENT_RE.sub(replacer, source)


def check_boundary(repo_root: Path) -> list[str]:
    violations = []
    flux_root = repo_root / "libs" / "flux"
    if not flux_root.is_dir():
        return [f"error: {flux_root} directory does not exist"]

    for root, dirs, files in os.walk(flux_root):
        dirs[:] = [d for d in dirs if d not in EXCLUDE_DIRS and not d.startswith(".")]
        for file in sorted(files):
            p = Path(root) / file
            ext = p.suffix
            is_meson = (p.name == "meson.build")
            if ext not in TARGET_EXTS and not is_meson:
                continue

            try:
                content = p.read_text(encoding="utf-8")
            except Exception as e:
                violations.append(f"{p}: cannot read file: {e}")
                continue

            # Strip comments to allow architectural commentary without false positives
            if is_meson:
                stripped = strip_meson_comments(content)
            else:
                stripped = strip_c_comments(content)

            orig_lines = content.splitlines()
            stripped_lines = stripped.splitlines()

            rel_path = p.relative_to(repo_root).as_posix()
            for line_no, (orig_line, stripped_line) in enumerate(zip(orig_lines, stripped_lines, strict=False), start=1):
                if FORBIDDEN_RE.search(stripped_line):
                    violations.append(f"{rel_path}:{line_no}:{orig_line.strip()}")

    return violations


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent.parent
    violations = check_boundary(repo_root)
    if violations:
        sys.stderr.write("error: material boundary violation — flux references a material name:\n")
        for v in violations:
            sys.stderr.write(f"{v}\n")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
