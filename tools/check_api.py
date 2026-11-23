#!/usr/bin/env python3
"""Public API shape guardrail.

The rule (docs/dev/standards/api-design.md):

  1. A public function takes at most four parameters. Anything that needs
     more configuration takes one descriptor (`const <lib>_<item>_opts *` or
     `_desc *`) instead of growing its parameter list.
  2. A function never takes two descriptors.
  3. No variant suffixes (`_ex`, `_wrapped`, ...): a variant is a descriptor
     field, not a second entry point.

Value kernels are exempt from (1): pure math and geometry functions whose
parameters *are* the operands (`gfx/math.h`, `gfx/curve.h`, `gfx/sdf.h`,
path verbs). Wrapping six floats in a struct there would only obscure them.

Existing violations are listed in tools/config/api_baseline.txt. The list is a
ratchet: a function not on it fails the check, and an entry that no longer
violates also fails until it is deleted, so the list can only shrink.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BASELINE = ROOT / "tools" / "config" / "api_baseline.txt"
MAX_PARAMS = 4

SUFFIX = re.compile(r"_(?:ex|wrapped|sized|primary|subtle|vertical)$")
DECL = re.compile(
    r"\b[A-Z0-9_]+_API\b[^;{]*?\b([a-z][a-z0-9_]*)\s*\(([^;{]*?)\)\s*;", re.DOTALL
)
DESCRIPTOR = re.compile(r"_(?:opts|desc)\s*\*")

# Headers whose functions are value kernels: operands, not configuration.
KERNEL_HEADERS = {"gfx/math.h", "gfx/curve.h", "gfx/sdf.h"}
# Path verbs take their control points positionally, like every 2D API.
# optics_gfx_console_logger is an implementation of the optics_gfx_log_fn callback type:
# its parameter list is that typedef's, not a choice made at the declaration.
KERNEL_FUNCTIONS = re.compile(r"^(?:optics_gfx_path_(?:quad|cubic)_to|optics_gfx_console_logger)$")


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", " ", text)


def split_params(params: str) -> list[str]:
    params = params.strip()
    if params in ("", "void"):
        return []
    out, depth, cur = [], 0, ""
    for ch in params:
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    out.append(cur.strip())
    return out


def scan() -> tuple[dict[str, str], list[str]]:
    """Returns ({function: reason} for baseline-eligible violations, hard errors)."""
    wide: dict[str, str] = {}
    errors: list[str] = []
    for header in sorted((ROOT / "libs").glob("*/include/**/*.h")):
        rel = header.relative_to(ROOT)
        key = "/".join(header.parts[-2:])
        text = strip_comments(header.read_text(encoding="utf-8", errors="replace"))
        for match in DECL.finditer(text):
            name, params = match.group(1), split_params(match.group(2))
            if SUFFIX.search(name):
                errors.append(f"{rel}: {name}: variant suffix; make it a descriptor field")
            if sum(1 for p in params if DESCRIPTOR.search(p)) > 1:
                errors.append(f"{rel}: {name}: takes more than one descriptor")
            if len(params) <= MAX_PARAMS or params[-1] == "...":
                continue
            if key in KERNEL_HEADERS or KERNEL_FUNCTIONS.match(name):
                continue
            wide[name] = f"{rel}: {name}: {len(params)} parameters (max {MAX_PARAMS}); use a descriptor"
    return wide, errors


def read_baseline() -> list[str]:
    if not BASELINE.exists():
        return []
    lines = BASELINE.read_text(encoding="utf-8").splitlines()
    return [ln.strip() for ln in lines if ln.strip() and not ln.startswith("#")]


def main() -> int:
    wide, errors = scan()
    baseline = read_baseline()
    for name, message in sorted(wide.items()):
        if name not in baseline:
            errors.append(message)
    for name in baseline:
        if name not in wide:
            errors.append(
                f"{BASELINE.relative_to(ROOT)}: {name} no longer violates; delete its entry"
            )

    if errors:
        sys.stderr.write("public API shape failed:\n")
        for error in errors:
            sys.stderr.write(f"  - {error}\n")
        return 1
    print(f"public API shape ok ({len(baseline)} baselined functions still to convert)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
