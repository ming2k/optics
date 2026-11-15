#!/usr/bin/env python3
"""C23 baseline guardrail.

The repository's C23 policy is explicit: use `nullptr`, not `NULL`; use `{}`,
not `{0}`; and give every `typedef enum` an explicit underlying type. This
scanner strips comments and string/character literals before matching, so prose
that describes the rule does not fail it.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def code_mask(text: str) -> bytearray:
    mask = bytearray(b"\x01") * len(text)
    i = 0
    state = "code"
    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""
        if state == "code":
            if ch == "/" and nxt == "/":
                mask[i] = mask[i + 1] = 0
                state = "line"
                i += 2
                continue
            if ch == "/" and nxt == "*":
                mask[i] = mask[i + 1] = 0
                state = "block"
                i += 2
                continue
            if ch == '"':
                mask[i] = 0
                state = "string"
                i += 1
                continue
            if ch == "'":
                mask[i] = 0
                state = "char"
                i += 1
                continue
            i += 1
            continue
        if state == "line":
            mask[i] = 0
            if ch == "\n":
                state = "code"
            i += 1
            continue
        if state == "block":
            mask[i] = 0
            if ch == "*" and nxt == "/":
                mask[i + 1] = 0
                state = "code"
                i += 2
                continue
            i += 1
            continue
        mask[i] = 0
        if ch == "\\" and i + 1 < len(text):
            mask[i + 1] = 0
            i += 2
            continue
        if (state == "string" and ch == '"') or (state == "char" and ch == "'"):
            state = "code"
        i += 1
    return mask


def line_of(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def main() -> int:
    errors: list[str] = []
    patterns = ("libs/**/*.c", "libs/**/*.h", "tests/**/*.c", "tests/**/*.h",
                "examples/**/*.c", "examples/**/*.h", "include/**/*.h")
    files: list[Path] = []
    for pattern in patterns:
        files.extend(sorted(ROOT.glob(pattern)))

    for path in files:
        text = path.read_text(encoding="utf-8", errors="replace")
        mask = code_mask(text)
        for match in re.finditer(r"\bNULL\b", text):
            if all(mask[match.start() : match.end()]):
                errors.append(f"{path.relative_to(ROOT)}:{line_of(text, match.start())}: NULL")
        for match in re.finditer(r"\{\s*0\s*\}", text):
            if all(mask[match.start() : match.end()]):
                errors.append(f"{path.relative_to(ROOT)}:{line_of(text, match.start())}: {{0}}")
        for match in re.finditer(r"\btypedef\s+enum\b", text):
            if not all(mask[match.start() : match.end()]):
                continue
            brace = text.find("{", match.end())
            if brace < 0:
                continue
            if ":" not in text[match.end() : brace]:
                errors.append(
                    f"{path.relative_to(ROOT)}:{line_of(text, match.start())}: "
                    "typedef enum without explicit underlying type"
                )

    if errors:
        sys.stderr.write("C23 lexical baseline failed:\n")
        for error in errors:
            sys.stderr.write(f"  - {error}\n")
        return 1

    print("C23 baseline ok: no NULL, no {0}, every typedef enum is typed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
