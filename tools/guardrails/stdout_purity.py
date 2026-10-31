#!/usr/bin/env python3
"""Library stdout purity guardrail.

Assures that shared library diagnostics in libs/ never write to stdout (e.g. printf).
Stdout is reserved strictly for consumer IPC channels. Diagnostics must go to stderr.
Syntax-aware: ignores comments mentioning printf.
"""

import os
import re
import sys
from pathlib import Path

PRINTF_CALL_RE = re.compile(r"(?:^|[^A-Za-z_0-9])printf\s*\(")
C_COMMENT_RE = re.compile(r"//[^\n]*|/\*.*?\*/", re.DOTALL)

TARGET_EXTS = {".c", ".h", ".m"}
EXCLUDE_DIRS = {".git", "build", "build-release", "build-tidy", "subprojects", "vendor"}


def strip_c_comments(source: str) -> str:
    def replacer(m):
        return "\n" * m.group(0).count("\n")
    return C_COMMENT_RE.sub(replacer, source)


def check_stdout_purity(repo_root: Path) -> list[str]:
    violations = []
    libs_root = repo_root / "libs"
    if not libs_root.is_dir():
        return [f"error: {libs_root} directory does not exist"]

    for root, dirs, files in os.walk(libs_root):
        dirs[:] = [d for d in dirs if d not in EXCLUDE_DIRS and not d.startswith(".")]
        for file in sorted(files):
            p = Path(root) / file
            if p.suffix not in TARGET_EXTS:
                continue

            try:
                content = p.read_text(encoding="utf-8")
            except Exception as e:
                violations.append(f"{p}: cannot read file: {e}")
                continue

            stripped = strip_c_comments(content)
            orig_lines = content.splitlines()
            stripped_lines = stripped.splitlines()

            rel_path = p.relative_to(repo_root).as_posix()
            for line_no, (orig_line, stripped_line) in enumerate(zip(orig_lines, stripped_lines, strict=False), start=1):
                if PRINTF_CALL_RE.search(stripped_line):
                    violations.append(f"{rel_path}:{line_no}:{orig_line.strip()}")

    return violations


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent.parent
    violations = check_stdout_purity(repo_root)
    if violations:
        sys.stderr.write("error: library source writes to stdout — diagnostics belong on stderr:\n")
        for v in violations:
            sys.stderr.write(f"{v}\n")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
