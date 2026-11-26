#!/usr/bin/env python3
"""Mechanical checks for the library purity red-lines.

1. Shared-library translation units under `libs/` may not write to stdout.
   Diagnostics go to stderr; stdout belongs to the application or IPC wire.
2. `libs/gfx` may not reference named materials or `optics_material_*`
   symbols. The Meson dependency graph already prevents a link; this catches a
   source-level reference before it becomes a build-file change.

Comments and string/character literals are stripped before matching so a
comment that *describes* the rule cannot fail the rule.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

STDOUT_RE = re.compile(
    r"(?<![A-Za-z0-9_])(?:printf|puts|putchar)\s*\("
    r"|fprintf\s*\(\s*stdout\b"
    r"|fputs\s*\([^,;]+,\s*stdout\b"
    r"|fwrite\s*\([^;]+,\s*stdout\b"
)
MATERIAL_RE = re.compile(
    r"\boptics_material_[A-Za-z0-9_]*"
    r"|\bprism_[A-Za-z0-9_]*"
    r"|\bliquid_glass\b"
    r"|\bfrosted_glass\b"
    r"|\bacrylic_[A-Za-z0-9_]*"
    r"|\bmica_[A-Za-z0-9_]*"
    r"|#\s*include\s*<optics/material(?:\.h|/)"
)


def strip_comments_and_literals(text: str) -> str:
    """Replace comments and string/char literals with spaces, preserving lines."""
    out: list[str] = []
    i = 0
    state = "code"
    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""
        if state == "code":
            if ch == "/" and nxt == "/":
                state = "line"
                out.extend("  ")
                i += 2
                continue
            if ch == "/" and nxt == "*":
                state = "block"
                out.extend("  ")
                i += 2
                continue
            if ch == '"':
                state = "string"
                out.append(" ")
                i += 1
                continue
            if ch == "'":
                state = "char"
                out.append(" ")
                i += 1
                continue
            out.append(ch)
            i += 1
            continue
        if state == "line":
            if ch == "\n":
                state = "code"
                out.append("\n")
            else:
                out.append(" ")
            i += 1
            continue
        if state == "block":
            if ch == "*" and nxt == "/":
                state = "code"
                out.extend("  ")
                i += 2
                continue
            out.append("\n" if ch == "\n" else " ")
            i += 1
            continue
        # string / char
        if ch == "\\" and i + 1 < len(text):
            out.extend("  ")
            i += 2
            continue
        if (state == "string" and ch == '"') or (state == "char" and ch == "'"):
            state = "code"
            out.append(" ")
            i += 1
            continue
        out.append("\n" if ch == "\n" else " ")
        i += 1
    return "".join(out)


def line_of(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def check_file(path: Path, pattern: re.Pattern[str], message: str) -> list[str]:
    raw = path.read_text(encoding="utf-8", errors="replace")
    code = strip_comments_and_literals(raw)
    errors: list[str] = []
    for match in pattern.finditer(code):
        errors.append(f"{path.relative_to(ROOT)}:{line_of(code, match.start())}: {message}")
    return errors


def main() -> int:
    errors: list[str] = []

    for path in sorted((ROOT / "libs").glob("*/src/**/*.c")):
        errors += check_file(path, STDOUT_RE, "shared library writes to stdout")

    for path in sorted((ROOT / "libs" / "gfx").glob("**/*.[ch]")):
        errors += check_file(path, MATERIAL_RE, "gfx references a named material")

    for path in sorted((ROOT / "libs" / "ui").glob("**/*.[ch]")):
        errors += check_file(path, MATERIAL_RE, "ui references a branded OS material")

    for forbidden_mod in ("svg", "scene", "material"):
        if (ROOT / "libs" / forbidden_mod).exists():
            errors.append(
                f"libs/{forbidden_mod}: retired non-core library must not exist in the core stack"
            )

    if errors:
        sys.stderr.write("library purity failed:\n")
        for error in errors:
            sys.stderr.write(f"  - {error}\n")
        return 1

    print("library purity ok: no stdout in libs, no branded materials or retired asset parsers")
    return 0


if __name__ == "__main__":
    sys.exit(main())
