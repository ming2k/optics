#!/usr/bin/env python3
"""Public API surface guardrail.

The single-descriptor policy is a migration ratchet: this scanner rejects the
named anti-pattern suffixes from new public declarations. It does not pretend
the older positional APIs are already converted; it prevents the suffix
proliferation that ADR-0082 removed from lens.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SUFFIX = re.compile(
    r"\b[a-z][a-z0-9_]*_(?:ex|wrapped|sized|primary|subtle|vertical)\s*\("
)


def main() -> int:
    errors: list[str] = []
    for header in sorted((ROOT / "libs").glob("*/include/**/*.h")):
        text = header.read_text(encoding="utf-8", errors="replace")
        text = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
        text = re.sub(r"//[^\n]*", " ", text)
        for match in SUFFIX.finditer(text):
            line = text.count("\n", 0, match.start()) + 1
            errors.append(f"{header.relative_to(ROOT)}:{line}: {match.group(0).strip()}")

    if errors:
        sys.stderr.write("public API surface failed:\n")
        for error in errors:
            sys.stderr.write(f"  - {error}\n")
        return 1

    print("public API surface ok: no prohibited descriptor suffixes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
