#!/usr/bin/env python3
"""Semantic module naming & unified optics namespace guardrail (ADR-0170).

Enforces mechanically that:
  1. Only canonical semantic module names exist under `libs/`, and no legacy
     abstract codenames (`flux`, `glyph`, `reticle`, `prism`, `vista`,
     `transit`, `iris`, `lens`, `aperture`) exist under `libs/`, `tests/`,
     `examples/`, or `crates/`.
  2. Every library installs public headers strictly under
     `libs/<mod>/include/optics/` with entry header `optics/<mod>.h`.
  3. Every public `OPTICS_<MOD>_API` function starts with `optics_<mod>_`.
  4. `libs/<mod>/meson.build` uses `optics_<mod>` for shared libraries and
     `optics-<mod>` for pkg-config filebases.
  5. `meson_options.txt` declares no legacy codename options.
  6. `crates/` contains only `optics`, `optics-build`, `optics-<mod>`, and
     `optics-<mod>-sys`.

Run standalone or through `meson test --suite lint`.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

CANONICAL_MODULES = frozenset(
    {"gfx", "text", "svg", "material", "scene", "motion", "window", "ui", "app"}
)
LEGACY_CODENAMES = frozenset(
    {"flux", "glyph", "reticle", "prism", "vista", "transit", "iris", "lens", "aperture"}
)

API_DECL_RE = re.compile(
    r"\b(OPTICS_([A-Z]+)_API)\b[^;{]*?\b([a-z][a-z0-9_]*)\s*\([^;{]*?\)\s*;",
    re.DOTALL,
)
LEGACY_API_RE = re.compile(
    r"\b(?:FLUX|GLYPH|RETICLE|PRISM|VISTA|TRANSIT|IRIS|LENS|APERTURE)_API\b"
)
OPTION_RE = re.compile(r"^\s*option\(\s*'([a-z0-9_]+)'", re.MULTILINE)
LIB_TARGET_RE = re.compile(r"\blibrary\(\s*'([a-z0-9_]+)'")
PKG_FILEBASE_RE = re.compile(r"\bfilebase\s*:\s*'([a-z0-9_-]+)'")


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", " ", text)
    # Strip preprocessor directives so `#define OPTICS_X_API` and `#if defined(...)` are ignored.
    return re.sub(r"^\s*#(?:\\\n|[^\n])*$", " ", text, flags=re.MULTILINE)


def check_tree(root: Path) -> list[str]:
    errors: list[str] = []

    libs_dir = root / "libs"
    if not libs_dir.is_dir():
        return ["libs/ directory is missing"]

    # 1. Canonical libs/ and no legacy dirs in libs/, tests/, examples/
    for entry in sorted(libs_dir.iterdir()):
        if not entry.is_dir():
            continue
        name = entry.name
        if name in LEGACY_CODENAMES:
            errors.append(f"libs/{name}: legacy codename directory forbidden (ADR-0170)")
        elif name not in CANONICAL_MODULES:
            errors.append(f"libs/{name}: non-canonical module name (not in ADR-0170 registry)")

    tests_dir = root / "tests"
    if tests_dir.is_dir():
        for entry in sorted(tests_dir.iterdir()):
            if entry.is_dir() and entry.name in LEGACY_CODENAMES:
                errors.append(f"tests/{entry.name}: legacy codename directory forbidden (ADR-0170)")

    examples_dir = root / "examples"
    if examples_dir.is_dir():
        allowed_examples = {"getting_started", "showcase"}
        for entry in sorted(examples_dir.iterdir()):
            if entry.is_dir() and entry.name not in allowed_examples:
                errors.append(
                    f"examples/{entry.name}: only 'getting_started' and 'showcase' allowed (ADR-0171)"
                )

    # 2. Unified include/optics/ layout and public API prefix per library
    for mod in sorted(CANONICAL_MODULES):
        mod_dir = libs_dir / mod
        if not mod_dir.is_dir():
            continue
        inc_dir = mod_dir / "include"
        if not inc_dir.is_dir():
            errors.append(f"libs/{mod}/include: missing include directory")
            continue

        top_entries = [p.name for p in sorted(inc_dir.iterdir())]
        if top_entries != ["optics"]:
            errors.append(
                f"libs/{mod}/include: must contain only 'optics/', found {top_entries}"
            )

        entry_hdr = inc_dir / "optics" / f"{mod}.h"
        if not entry_hdr.is_file():
            errors.append(f"libs/{mod}/include/optics/{mod}.h: missing canonical entry header")

        expected_prefix = f"optics_{mod}_"
        expected_api = f"OPTICS_{mod.upper()}_API"
        stutter_lower_re = re.compile(rf"\boptics_{mod}_{mod}(?:_[a-z0-9_]+)?\b")
        stutter_upper_re = re.compile(rf"\bOPTICS_{mod.upper()}_{mod.upper()}_[A-Z0-9_]+\b")
        for hdr in sorted(inc_dir.rglob("*.h")):
            rel = hdr.relative_to(root).as_posix()
            if mod == "window" and hdr.name == "app.h":
                errors.append(
                    f"{rel}: libs/window must not own app.h; use window/loop.h (ADR-0172)"
                )
            raw_text = hdr.read_text(encoding="utf-8", errors="replace")
            text = strip_comments(raw_text)
            for m in LEGACY_API_RE.finditer(text):
                errors.append(f"{rel}: legacy API macro {m.group(0)} forbidden (ADR-0170)")
            for m in stutter_lower_re.finditer(text):
                errors.append(
                    f"{rel}: stuttered public identifier {m.group(0)} forbidden (ADR-0172)"
                )
            for m in stutter_upper_re.finditer(text):
                errors.append(
                    f"{rel}: stuttered public macro {m.group(0)} forbidden (ADR-0172)"
                )
            if mod == "window":
                for m in re.finditer(r"\boptics_window_app_[a-z0-9_]+\b", text):
                    errors.append(
                        f"{rel}: {m.group(0)} leaks 'app' into libs/window (ADR-0172)"
                    )
            for m in API_DECL_RE.finditer(text):
                api_macro, _mod_upper, fn_name = m.group(1), m.group(2), m.group(3)
                if api_macro != expected_api:
                    errors.append(
                        f"{rel}: {fn_name} uses {api_macro}, expected {expected_api}"
                    )
                if not fn_name.startswith(expected_prefix):
                    errors.append(
                        f"{rel}: public function {fn_name} must start with {expected_prefix}"
                    )

        # 3. Meson library & pkg-config naming
        meson_file = mod_dir / "meson.build"
        if meson_file.is_file():
            mtext = meson_file.read_text(encoding="utf-8", errors="replace")
            for m in LIB_TARGET_RE.finditer(mtext):
                lib_name = m.group(1)
                if lib_name != f"optics_{mod}":
                    errors.append(
                        f"libs/{mod}/meson.build: library('{lib_name}') must be library('optics_{mod}')"
                    )
            for m in PKG_FILEBASE_RE.finditer(mtext):
                fb = m.group(1)
                if fb != f"optics-{mod}":
                    errors.append(
                        f"libs/{mod}/meson.build: pkg-config filebase '{fb}' must be 'optics-{mod}'"
                    )

    # 4. meson_options.txt
    opts_file = root / "meson_options.txt"
    if opts_file.is_file():
        otext = opts_file.read_text(encoding="utf-8", errors="replace")
        for m in OPTION_RE.finditer(otext):
            opt_name = m.group(1)
            if opt_name in LEGACY_CODENAMES:
                errors.append(
                    f"meson_options.txt: legacy option '{opt_name}' forbidden (ADR-0170)"
                )

    # 5. Rust crates/ namespace
    crates_dir = root / "crates"
    if crates_dir.is_dir():
        allowed_crates = {"optics", "optics-build"}
        for mod in CANONICAL_MODULES:
            allowed_crates.add(f"optics-{mod}")
            allowed_crates.add(f"optics-{mod}-sys")
        for entry in sorted(crates_dir.iterdir()):
            if not entry.is_dir():
                continue
            if entry.name not in allowed_crates:
                errors.append(
                    f"crates/{entry.name}: non-canonical crate name (ADR-0170)"
                )

    # 6. Live documentation drift check (excludes immutable historical ADRs and dated reviews)
    live_doc_drift_re = re.compile(
        r"<(?:flux|glyph|reticle|prism|vista|transit|iris|lens|aperture)/"
        r"|examples/(?:flux|glyph|reticle|prism|vista|transit|iris|lens|aperture|scenarios)/"
        r"|libs/(?:flux|glyph|reticle|prism|vista|transit|iris|lens|aperture)/"
        r"|crates/optics-(?:flux|glyph|reticle|prism|vista|transit|iris|lens|aperture)\b"
        r"|-D(?:glyph|reticle|prism|vista|transit)="
        r"|\b(?:aperture_app_run|iris_app_run|iris_window_|optics_app_app_|optics_window_window_|optics_window_app_|optics_scene_scene_)"
    )
    historical_docs = {"docs/dev/architecture-review-2026-09-14.md"}
    for d in ("docs/tutorials", "docs/how-to", "docs/reference", "docs/explanation", "docs/dev"):
        doc_dir = root / d
        if not doc_dir.is_dir():
            continue
        for md in sorted(doc_dir.rglob("*.md")):
            rel = md.relative_to(root).as_posix()
            if rel in historical_docs:
                continue
            text = md.read_text(encoding="utf-8", errors="replace")
            m = live_doc_drift_re.search(text)
            if m:
                line = text.count("\n", 0, m.start()) + 1
                errors.append(
                    f"{rel}:{line}: stale legacy reference '{m.group(0)}' in live doc (ADR-0170/0171/0172)"
                )

    return errors


def main() -> int:
    root = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent
    errors = check_tree(root)
    if errors:
        sys.stderr.write("semantic naming guardrail failed:\n")
        for err in errors:
            sys.stderr.write(f"  - {err}\n")
        return 1
    print("semantic naming ok: all libs, headers, APIs, pkg-config, and crates match ADR-0170")
    return 0


if __name__ == "__main__":
    sys.exit(main())
