#!/usr/bin/env python3
"""ADR registry guardrail (docs/dev/standards/adr.md).

Every ADR carries the full frontmatter schema; a superseded ADR names an
existing successor and any other ADR says `superseded_by: null`; and the index
row agrees with the frontmatter. Before this check the index and the records
disagreed on a dozen ADRs, so neither could be trusted on its own.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
KEYS = ("id", "title", "status", "date", "scope", "superseded_by", "negative_knowledge")
STATUSES = frozenset({"draft", "accepted", "superseded", "rejected"})
FRONTMATTER_RE = re.compile(r"\A---\n(.*?)\n---\n", re.DOTALL)


def check(root: Path) -> list[str]:
    adr_dir = root / "docs" / "adr"
    errors: list[str] = []
    numbers = {p.name[:4] for p in adr_dir.glob("[0-9][0-9][0-9][0-9]-*.md")}
    index = (adr_dir / "index.md").read_text(encoding="utf-8")
    rows = {m.group(1): m.group(2).strip()
            for m in re.finditer(r"^\| (\d{4}) \|.*\| *([^|]+?) *\|\s*$", index, re.MULTILINE)}
    for p in sorted(adr_dir.glob("[0-9][0-9][0-9][0-9]-*.md")):
        rel = p.relative_to(root).as_posix()
        num = p.name[:4]
        m = FRONTMATTER_RE.match(p.read_text(encoding="utf-8"))
        if not m:
            errors.append(f"{rel}: missing YAML frontmatter")
            continue
        fm = {}
        for line in m.group(1).splitlines():
            key, sep, value = line.partition(":")
            if sep:
                fm[key.strip()] = value.strip()
        for key in KEYS:
            if not fm.get(key):
                errors.append(f"{rel}: frontmatter lacks '{key}'")
        if fm.get("id") != f"ADR-{num}":
            errors.append(f"{rel}: id '{fm.get('id')}' does not match the file number")
        status = fm.get("status", "")
        successor = fm.get("superseded_by", "")
        if status not in STATUSES:
            errors.append(f"{rel}: status '{status}' is not one of {sorted(STATUSES)}")
        if status == "superseded":
            sm = re.fullmatch(r"ADR-(\d{4})", successor)
            if not sm or sm.group(1) not in numbers:
                errors.append(f"{rel}: superseded ADR must name an existing successor, got '{successor}'")
        elif successor != "null":
            errors.append(f"{rel}: status '{status}' requires superseded_by: null")
        row = rows.get(num)
        if row is None:
            errors.append(f"docs/adr/index.md: no row for ADR-{num}")
        elif status == "superseded":
            if not row.startswith("Superseded") or successor[4:] not in row:
                errors.append(f"docs/adr/index.md: ADR-{num} row '{row}' disagrees with {successor}")
        elif not row.lower().startswith(status):
            errors.append(f"docs/adr/index.md: ADR-{num} row '{row}' disagrees with status '{status}'")
    return errors


def main() -> int:
    errors = check(ROOT)
    if errors:
        sys.stderr.write("ADR registry check failed:\n")
        for e in errors:
            sys.stderr.write(f"  - {e}\n")
        return 1
    print("ADR registry ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
