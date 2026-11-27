#!/usr/bin/env python3
"""Mechanical checks for library purity and public packaging red-lines.

1. Shared-library translation units under `libs/` may not write to stdout.
   Diagnostics go to stderr; stdout belongs to the application or IPC wire.
2. `libs/gfx` and `libs/ui` may not reference retired branded OS materials or
   `optics_material_*` symbols, and no library may reference the retired
   top-level `motion` library or keep retired library directories.
3. Every public header / shader snippet under `include/` and `libs/*/include/`
   must be listed in `install_headers(...)` in its owning `meson.build`, so
   `meson install` can never ship a broken include tree.
4. Every public header declaring `OPTICS_<MOD>_API` or `OPTICS_<MOD>_INLINE`
   symbols must carry an `extern "C"` guard for C++ consumers.

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
RETIRED_MOTION_RE = re.compile(
    r"\boptics_motion_[A-Za-z0-9_]*"
    r"|\bOPTICS_MOTION_[A-Za-z0-9_]*"
    r"|#\s*include\s*<optics/motion(?:\.h|/)"
)
PUBLIC_SYMBOL_DECL_RE = re.compile(
    r"\bOPTICS_[A-Z]+_(?:API|INLINE)\b[^;{]*?\boptics_[a-z]+_[a-z0-9_]+\s*\("
)
INSTALL_HEADERS_BLOCK_RE = re.compile(r"install_headers\s*\((.*?)\)", re.DOTALL)
STRING_LITERAL_RE = re.compile(r"'([^']+)'")


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


def installed_header_paths(meson_file: Path) -> set[str]:
    text = meson_file.read_text(encoding="utf-8")
    installed: set[str] = set()
    for block in INSTALL_HEADERS_BLOCK_RE.findall(text):
        for literal in STRING_LITERAL_RE.findall(block):
            if literal.startswith("include/"):
                installed.add(literal)
    return installed


def check_installed_headers() -> list[str]:
    errors: list[str] = []

    root_installed = installed_header_paths(ROOT / "meson.build")
    for path in sorted((ROOT / "include").rglob("*")):
        if not path.is_file() or path.suffix not in {".h", ".slang"}:
            continue
        rel = path.relative_to(ROOT).as_posix()
        if rel not in root_installed:
            errors.append(f"{rel}: public header missing from meson.build install_headers()")

    for lib_dir in sorted(p for p in (ROOT / "libs").iterdir() if p.is_dir()):
        meson_file = lib_dir / "meson.build"
        inc_dir = lib_dir / "include"
        if not meson_file.is_file() or not inc_dir.is_dir():
            continue
        lib_installed = installed_header_paths(meson_file)
        for path in sorted(inc_dir.rglob("*")):
            if not path.is_file() or path.suffix not in {".h", ".slang"}:
                continue
            rel = path.relative_to(lib_dir).as_posix()
            if rel not in lib_installed:
                errors.append(
                    f"{path.relative_to(ROOT)}: public header missing from "
                    f"{meson_file.relative_to(ROOT)} install_headers()"
                )
    return errors


def check_extern_c_guards() -> list[str]:
    errors: list[str] = []
    headers = list((ROOT / "include").rglob("*.h")) + list((ROOT / "libs").glob("*/include/**/*.h"))
    for path in sorted(headers):
        raw = path.read_text(encoding="utf-8", errors="replace")
        code = strip_comments_and_literals(raw)
        if PUBLIC_SYMBOL_DECL_RE.search(code) and 'extern "C"' not in raw:
            errors.append(
                f"{path.relative_to(ROOT)}: declares public symbols without an extern \"C\" guard"
            )
    return errors


def main() -> int:
    errors: list[str] = []

    for path in sorted((ROOT / "libs").glob("*/src/**/*.c")):
        errors += check_file(path, STDOUT_RE, "shared library writes to stdout")

    for path in sorted((ROOT / "libs" / "gfx").glob("**/*.[ch]")):
        errors += check_file(path, MATERIAL_RE, "gfx references a named material")

    for path in sorted((ROOT / "libs" / "ui").glob("**/*.[ch]")):
        errors += check_file(path, MATERIAL_RE, "ui references a branded OS material")

    for path in sorted((ROOT / "libs").glob("**/*.[ch]")):
        errors += check_file(
            path,
            RETIRED_MOTION_RE,
            "references retired top-level motion library (use <optics/ui/motion.h>)",
        )

    for forbidden_mod in ("svg", "scene", "material", "motion"):
        if (ROOT / "libs" / forbidden_mod).exists():
            errors.append(
                f"libs/{forbidden_mod}: retired top-level library must not exist in the core stack"
            )

    errors += check_installed_headers()
    errors += check_extern_c_guards()

    if errors:
        sys.stderr.write("library purity failed:\n")
        for error in errors:
            sys.stderr.write(f"  - {error}\n")
        return 1

    print(
        "library purity ok: no stdout in libs, no branded materials or retired top-level "
        "libraries, all public headers installed with extern \"C\" guards"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
