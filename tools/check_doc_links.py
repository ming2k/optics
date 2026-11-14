#!/usr/bin/env python3
"""Relative-link and audience-firewall checker for Markdown documentation.

Checks every local link in `docs/`, `README.md`, and `AGENTS.md`:

- the target exists (anchors are stripped before resolution);
- public documentation (`docs/{tutorials,how-to,reference,explanation}/`) does
  not link into the internal maintainer surface (`docs/dev/`).

External URLs, mail links, and bare anchors are ignored. Fenced code blocks are
skipped so an example command cannot be mistaken for a document link.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parent.parent
LINK_RE = re.compile(r"\[[^\]]*\]\(([^)\n]+)\)")
FENCE_RE = re.compile(r"^\s*(```|~~~)")
PUBLIC_DIRS = {"tutorials", "how-to", "reference", "explanation"}


def markdown_files() -> list[Path]:
    files = [ROOT / "README.md", ROOT / "AGENTS.md"]
    files += sorted((ROOT / "docs").glob("**/*.md"))
    return [path for path in files if path.is_file()]


def visible_lines(text: str) -> list[tuple[int, str]]:
    lines: list[tuple[int, str]] = []
    fenced = False
    for number, line in enumerate(text.splitlines(), start=1):
        if FENCE_RE.match(line):
            fenced = not fenced
            continue
        if not fenced:
            lines.append((number, line))
    return lines


def link_target(raw: str) -> str | None:
    target = raw.strip()
    if target.startswith("<") and ">" in target:
        target = target[1 : target.index(">")]
    else:
        target = target.split(maxsplit=1)[0]
    if not target or target.startswith("#"):
        return None
    lowered = target.lower()
    if lowered.startswith(("http://", "https://", "mailto:", "tel:", "data:")):
        return None
    return unquote(target.split("#", 1)[0])


def main() -> int:
    errors: list[str] = []
    for source in markdown_files():
        text = source.read_text(encoding="utf-8")
        source_public = source.parent.name in PUBLIC_DIRS and source.parent.parent == ROOT / "docs"
        for number, line in visible_lines(text):
            for match in LINK_RE.finditer(line):
                target = link_target(match.group(1))
                if target is None:
                    continue
                resolved = (source.parent / target).resolve()
                try:
                    resolved.relative_to(ROOT)
                except ValueError:
                    errors.append(
                        f"{source.relative_to(ROOT)}:{number}: link escapes the repository: {target}"
                    )
                    continue
                if not resolved.exists():
                    errors.append(
                        f"{source.relative_to(ROOT)}:{number}: missing link target: {target}"
                    )
                    continue
                if source_public:
                    try:
                        relative = resolved.relative_to(ROOT)
                    except ValueError:
                        continue
                    if len(relative.parts) >= 2 and relative.parts[0] == "docs" and relative.parts[1] == "dev":
                        errors.append(
                            f"{source.relative_to(ROOT)}:{number}: public doc links into docs/dev: {target}"
                        )

    if errors:
        sys.stderr.write("documentation links failed:\n")
        for error in errors:
            sys.stderr.write(f"  - {error}\n")
        return 1

    print("documentation links ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
