#!/usr/bin/env python3
"""symbols.py — generate docs/reference/symbols.md from installed headers.

The headers are the single source of truth for the public API surface. This
module extracts every exported function from the headers each library's
meson.build actually installs, pairs each with the first sentence of its
doc comment, and emits the symbol lookup table.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# (library title, header paths relative to repo root, API macro)
LIBRARIES = [
    (
        "flux — rendering engine",
        [
            "libs/flux/include/flux/core.h",
            "libs/flux/include/flux/math.h",
            "libs/flux/include/flux/vulkan.h",
            "libs/flux/include/flux/canvas.h",
            "libs/flux/include/flux/canvas_cpu.h",
            "libs/flux/include/flux/dmabuf.h",
            "libs/flux/include/flux/profile.h",
            "libs/flux/include/flux/scene.h",
            "libs/flux/include/flux/compute.h",
            "libs/flux/include/flux/effect.h",
            "libs/flux/include/flux/sdf.h",
            "libs/flux/include/flux/curve.h",
            "libs/flux/include/flux/observe.h",
            "libs/flux/include/flux/observe_sites.h",
        ],
        r"FLUX_API",
    ),
    (
        "glyph — text shaping & layout engine",
        ["libs/glyph/include/glyph/glyph.h"],
        r"GLYPH_API",
    ),
    (
        "vista — 3D scene & skeletal animation engine",
        ["libs/vista/include/vista/vista.h"],
        r"VISTA_API",
    ),
    (
        "lens — immediate-mode UI",
        [
            "libs/lens/include/lens/lens.h",
            "libs/lens/include/lens/icon.h",
            "libs/lens/include/lens/material.h",
            "libs/lens/include/lens/patterns.h",
        ],
        r"LENS_API",
    ),
    (
        "iris — application toolkit",
        [
            "libs/iris/include/iris/app.h",
            "libs/iris/include/iris/event.h",
            "libs/iris/include/iris/iris.h",
            "libs/iris/include/iris/window.h",
            "libs/iris/include/iris/cursor.h",
            "libs/iris/include/iris/theme.h",
            "libs/iris/include/iris/file_dialog.h",
            "libs/iris/include/iris/capability.h",
            "libs/iris/include/iris/a11y.h",
            "libs/iris/include/iris/a11y_prefs.h",
            "libs/iris/include/iris/dnd.h",
        ],
        r"IRIS_API",
    ),
    (
        "prism — material library",
        [
            "libs/prism/include/prism/types.h",
            "libs/prism/include/prism/liquid_glass.h",
            "libs/prism/include/prism/frosted.h",
            "libs/prism/include/prism/acrylic.h",
            "libs/prism/include/prism/mica.h",
            "libs/prism/include/prism/backdrop_layer.h",
        ],
        r"PRISM_API",
    ),
    (
        "transit — motion vocabulary",
        ["libs/transit/include/transit/transit.h"],
        r"TRANSIT_API",
    ),
    (
        "svg — vector-document compiler",
        ["libs/svg/include/svg/svg.h"],
        r"SVG_API",
    ),
]

DECL = re.compile(
    r"(?:^|\n)(?P<comment>(?:/\*(?:[^*]|\*(?!/))*\*/\s*)*)"  # doc comment(s)
    r"(?P<attr>(?:[A-Z_]+(?:\([^)]*\))?\s*)*)"
    r"(?P<api>{api})\s+"
    r"(?P<ret>[A-Za-z_][A-Za-z0-9_ *]*?)\s+"
    r"(?P<name>[a-z][a-z0-9_]*)\s*\(",
)


def first_sentence(comment: str) -> str:
    blocks = re.findall(r"/\*.*?\*/", comment, flags=re.S)
    if blocks:
        comment = blocks[-1]
    text = re.sub(r"^/\*+|\*+/$", "", comment.strip(), flags=re.S)
    text = re.sub(r"^\s*\*\s?", "", text, flags=re.M)
    text = " ".join(text.split())
    if not text:
        return ""
    m = re.match(r"(.+?(?<!\b[A-Z])[.!?])(?:\s|$)", text)
    sentence = m.group(1) if m else text
    return sentence.strip()


def _belongs_to_library(rel: str, api_macro: str) -> bool:
    """True when `rel` lives in the library that owns `api_macro`.

    Derived from the LIBRARIES table rather than a path heuristic so a new
    library adds no second place to forget."""
    for _, headers, api in LIBRARIES:
        if api != api_macro or not headers:
            continue
        lib_root = headers[0].rsplit("/include/", 1)[0]
        if rel.startswith(lib_root + "/"):
            return True
    return False


def extract(header: Path, api_macro: str):
    src = header.read_text(encoding="utf-8")
    pattern = DECL.pattern.replace("{api}", api_macro)
    seen, rows = set(), []
    for m in re.finditer(pattern, src, flags=re.S):
        name = m.group("name")
        if name in seen:
            continue
        seen.add(name)
        desc = first_sentence(m.group("comment"))
        if not desc:
            tail = src[m.end():]
            tm = re.match(r'\s*(/\*(?:[^*]|\*(?!/))*\*/)', tail)
            if tm and "\n\n" not in tail[: tm.end()]:
                desc = first_sentence(tm.group(1))
        rows.append((name, desc))
    return rows


def render(root: Path) -> str:
    lines = [
        "# Symbol Reference",
        "",
        "Lookup table for every exported function, grouped by library and",
        "header. GENERATED by `tools/guardrails/symbols.py` from the installed",
        "headers — the headers are the single source of truth; regenerate",
        "with `./tools/gate fix` after changing a public",
        "signature (CI fails on drift). For the shape of the API (object",
        "model, lifecycle, error model), see [API Reference](api.md); for",
        "blocking behavior per call, see [Thread Safety](thread-safety.md).",
        "",
    ]
    for title, headers, api in LIBRARIES:
        lines += [f"## {title}", ""]
        for h in headers:
            rows = extract(root / h, api)
            if not rows:
                continue
            lines += [f"### `{h.split('include/')[-1]}`", ""]
            lines += ["| Symbol | Description |", "|--------|-------------|"]
            for name, desc in rows:
                d = desc.replace("|", "\\|") if desc else ""
                lines.append(f"| `{name}` | {d} |")
            lines.append("")
    lines.append(
        "Every fallible function returns `flux_result`-style codes; see"
        " [API Reference](api.md#error-model). Functions returning `void`"
        " either cannot fail or report through the library's error channel"
        " as noted.\n"
    )
    return "\n".join(lines)


def run_symbols_check(root: Path, check_only: bool = True) -> tuple[bool, str, str]:
    out_file = root / "docs/reference/symbols.md"

    # Completeness gate (ADR-0095 symbol index): every installed header that
    # exports the library's API macro must be listed above. Without this the
    # guardrail only verified that a hand-maintained list was regenerated, so a
    # whole header could ship exported-and-undocumented indefinitely — which is
    # exactly what happened to flux/sdf.h (7 symbols) and flux/curve.h (20).
    listed = {h for _, headers, _ in LIBRARIES for h in headers}
    uncovered: list[str] = []
    for lib_title, _, api in LIBRARIES:
        for header in sorted(root.glob("libs/*/include/*/*.h")):
            rel = header.relative_to(root).as_posix()
            if rel in listed:
                continue
            if not _belongs_to_library(rel, api):
                continue
            text = header.read_text(encoding="utf-8")
            if not re.search(rf"\b{api}\b", text):
                continue
            # A header that only *defines* the export macro declares no symbols;
            # every library has one and none belongs in a symbol index.
            if re.search(rf"#\s*define\s+{api}\b", text):
                continue
            uncovered.append(rel)
    if uncovered:
        return (
            False,
            "",
            "symbols guardrail: these headers export a public API but are not "
            "listed in tools/guardrails/symbols.py, so their symbols are absent "
            "from docs/reference/symbols.md:\n"
            + "\n".join(f"  {h}" for h in uncovered),
        )

    for _, headers, _ in LIBRARIES:
        for h in headers:
            if not (root / h).exists():
                return False, "", f"symbols guardrail: header missing: {h}"

    fresh = render(root)
    if check_only:
        current = out_file.read_text(encoding="utf-8") if out_file.exists() else ""
        if current != fresh:
            return (
                False,
                "",
                "docs/reference/symbols.md is stale. Regenerate with:\n    ./tools/gate fix",
            )
        return True, "symbols.md: fresh and complete", ""

    out_file.write_text(fresh, encoding="utf-8")
    return True, f"wrote {out_file.relative_to(root)} ({len(fresh.splitlines())} lines)", ""


def main() -> int:
    parser = argparse.ArgumentParser(description="Extract public C symbols into docs/reference/symbols.md")
    parser.add_argument("--check", action="store_true", help="fail if symbols.md is stale")
    args = parser.parse_args()

    root = Path(__file__).resolve().parent.parent.parent
    ok, stdout_msg, stderr_msg = run_symbols_check(root, check_only=args.check)
    if ok:
        print(stdout_msg)
        return 0
    sys.stderr.write(stderr_msg + "\n")
    return 1


if __name__ == "__main__":
    sys.exit(main())
