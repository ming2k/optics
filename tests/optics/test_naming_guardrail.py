#!/usr/bin/env python3
"""Negative regression tests for tools/check_naming.py (ADR-0170)."""

from __future__ import annotations

import importlib.util
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
SPEC = importlib.util.spec_from_file_location("check_naming", ROOT / "tools" / "check_naming.py")
assert SPEC and SPEC.loader
MOD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MOD)


def make_valid_tree(root: Path) -> None:
    for mod in ("gfx", "ui"):
        inc = root / "libs" / mod / "include" / "optics"
        inc.mkdir(parents=True)
        (inc / f"{mod}.h").write_text(
            f"OPTICS_{mod.upper()}_API void optics_{mod}_init(void);\n",
            encoding="utf-8",
        )
        (root / "libs" / mod / "meson.build").write_text(
            f"library('optics_{mod}', 'a.c')\npkg.generate(filebase : 'optics-{mod}')\n",
            encoding="utf-8",
        )
    (root / "meson_options.txt").write_text("option('text', type : 'boolean')\n", encoding="utf-8")
    (root / "crates" / "optics").mkdir(parents=True)
    (root / "crates" / "optics-gfx").mkdir(parents=True)


def assert_fails_with(root: Path, needle: str) -> None:
    errors = MOD.check_tree(root)
    assert any(needle in e for e in errors), f"expected error containing {needle!r}, got {errors}"


def main() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp)
        make_valid_tree(base)
        assert MOD.check_tree(base) == [], f"valid tree failed: {MOD.check_tree(base)}"

    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp)
        make_valid_tree(base)
        (base / "libs" / "flux").mkdir(parents=True)
        assert_fails_with(base, "libs/flux: legacy codename")

    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp)
        make_valid_tree(base)
        (base / "libs" / "gfx" / "include" / "flux").mkdir(parents=True)
        assert_fails_with(base, "must contain only 'optics/'")

    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp)
        make_valid_tree(base)
        (base / "libs" / "ui" / "include" / "optics" / "ui.h").write_text(
            "OPTICS_UI_API void lens_begin(void);\n", encoding="utf-8"
        )
        assert_fails_with(base, "must start with optics_ui_")

    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp)
        make_valid_tree(base)
        (base / "crates" / "optics-lens").mkdir(parents=True)
        assert_fails_with(base, "crates/optics-lens: non-canonical crate name")

    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp)
        make_valid_tree(base)
        (base / "meson_options.txt").write_text("option('glyph', type : 'boolean')\n", encoding="utf-8")
        assert_fails_with(base, "legacy option 'glyph'")

    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp)
        make_valid_tree(base)
        (base / "examples" / "toys").mkdir(parents=True)
        assert_fails_with(base, "only 'getting_started' and 'showcase' allowed")

    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp)
        make_valid_tree(base)
        (base / "libs" / "ui" / "include" / "optics" / "ui.h").write_text(
            "OPTICS_UI_API void optics_ui_ui_begin(void);\n", encoding="utf-8"
        )
        assert_fails_with(base, "stuttered public identifier optics_ui_ui_begin")

    print("naming guardrail selftest: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
