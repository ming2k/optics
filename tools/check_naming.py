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

    live = live_files(root)
    errors += check_legacy_codenames(root, live)
    errors += check_dangling_references(root, live)
    errors += check_dangling_paths(root, live)
    return errors


# ----------------------------------------------------------------------------
# 6-8. Live-tree content (ADR-0170, ADR-0173)
#
# The rename is only finished when the codenames are gone from what people read
# and what the program prints, not just from directory names and API macros.
# "Live" is every source, build, tool and test file plus the current docs; ADRs
# and the CHANGELOG are history and keep the names they were written with.
# ----------------------------------------------------------------------------

LIVE_TREES = ("libs", "include", "tests", "examples", "crates", "tools", ".github")
LIVE_DOC_DIRS = ("docs/tutorials", "docs/how-to", "docs/reference", "docs/explanation",
                 "docs/dev")
LIVE_TOP_FILES = ("meson.build", "meson_options.txt", "README.md", "AGENTS.md", "docs/index.md")
LIVE_EXTS = frozenset({".c", ".h", ".m", ".rs", ".slang", ".py", ".build", ".txt", ".toml", ".sh",
                       ".md", ".yml", ".supp"})
# Guardrails and their regression tests hold codenames, stale names and fake
# paths as fixture data; the content rules do not apply to them.
GUARDRAIL_FILE_RE = re.compile(r"^(?:tools/check_[a-z_]+\.py|tests/optics/test_[a-z_]+_guardrail\.py)$")


def is_guardrail_file(rel: str) -> bool:
    return GUARDRAIL_FILE_RE.match(rel) is not None
# Dated, deliberately frozen records inside docs/dev.
HISTORICAL_DOCS = frozenset({"docs/dev/architecture-review-2026-09-14.md"})
SKIP_DIRS = frozenset({"target", "build", ".git", "subprojects"})

C_LIKE = frozenset({".c", ".h", ".m", ".rs", ".slang"})
CODE_EXTS = C_LIKE


def live_files(root: Path) -> list[Path]:
    out: list[Path] = []
    for d in LIVE_TREES + LIVE_DOC_DIRS:
        base = root / d
        if not base.is_dir():
            continue
        for p in sorted(base.rglob("*")):
            if not p.is_file() or p.suffix not in LIVE_EXTS:
                continue
            if SKIP_DIRS.intersection(p.relative_to(root).parts):
                continue
            out.append(p)
    for f in LIVE_TOP_FILES:
        if (root / f).is_file():
            out.append(root / f)
    return [p for p in out if p.relative_to(root).as_posix() not in HISTORICAL_DOCS]


def read(p: Path) -> str:
    return p.read_text(encoding="utf-8", errors="replace")


def line_of(text: str, pos: int) -> int:
    return text.count("\n", 0, pos) + 1


# Spans that legitimately contain a codename: ADR file names (links to history)
# and the Feather "aperture" icon.
PROTECTED_RE = re.compile(
    r"\b\d{4}-[a-z0-9]+(?:-[a-z0-9]+)*(?:\.md)?\b"
    r"|\b\w*icon_aperture\w*\b|\b\w*ICON_APERTURE\w*\b|\"aperture\"|aperture\.svg"
)

# flux, iris, aperture, reticle, vista and prism have no meaning in this tree
# other than the old module names, in any position: a word, a path segment, a
# string, or one `_`-separated part of an identifier.
UNAMBIGUOUS_CODENAME_RE = re.compile(
    r"(?<![A-Za-z0-9])(flux|iris|aperture|reticle|vista|prism)(?![A-Za-z0-9])", re.IGNORECASE
)
# ...and as the leading word of a CamelCase type or class name (FluxDiagnostic,
# IrisView). Glyph is excluded: GlyphRun is a glyph run.
CAMEL_CODENAME_RE = re.compile(r"(?<![A-Za-z0-9])(?:Flux|Iris|Aperture|Reticle|Vista|Prism|Lens|Transit)(?=[A-Z])")
# lens, glyph and transit are also ordinary words ("the glass lens", "a glyph
# run", "still in transit"), so only their codename forms are flagged: the old
# symbol prefixes, include paths, library and crate names, owner strings, and
# the module name in code quotes.
AMBIGUOUS_CODENAME_RE = re.compile(
    r"(?<![A-Za-z0-9_])(?:lensi?_|LENSI?_|transit_|TRANSIT_|GLYPH_[A-Z])"
    r"|<(?:lens|glyph|transit)/"
    r"|\b(?:libs|tests|examples)/(?:lens|glyph|transit)\b"
    r"|\blib(?:lens|glyph|transit)\b"
    r"|\boptics-(?:lens|glyph|transit)\b"
    r"|\"(?:lens|glyph|transit)[/.]"
    r"|`(?:lens|glyph|transit)`"
)


def check_legacy_codenames(root: Path, files: list[Path]) -> list[str]:
    errors: list[str] = []
    for p in files:
        rel = p.relative_to(root).as_posix()
        if is_guardrail_file(rel):
            continue
        text = PROTECTED_RE.sub(lambda m: " " * len(m.group(0)), read(p))
        for rx in (UNAMBIGUOUS_CODENAME_RE, AMBIGUOUS_CODENAME_RE, CAMEL_CODENAME_RE):
            for m in rx.finditer(text):
                errors.append(
                    f"{rel}:{line_of(text, m.start())}: legacy codename '{m.group(0)}'"
                    " (ADR-0170; use the semantic module name)"
                )
    return errors


# An `optics_*` / `OPTICS_*` name mentioned in a comment or a doc must exist in
# code. This is what keeps prose from describing functions that were renamed or
# deleted. Names assembled at run time are listed explicitly.
OPTICS_NAME_RE = re.compile(r"\b(?:optics|OPTICS)_[A-Za-z0-9]+_[A-Za-z0-9_]*[A-Za-z0-9]\b")
RUNTIME_NAMES_RE = re.compile(r"^OPTICS_[A-Z]+_SOURCE_DIR$")  # optics-build: format!("{}_SOURCE_DIR")
C_COMMENT_RE = re.compile(r"/\*.*?\*/|//[^\n]*", re.DOTALL)
HASH_COMMENT_RE = re.compile(r"#[^\n]*")


def check_dangling_references(root: Path, files: list[Path]) -> list[str]:
    defined: set[str] = set()
    mentions: list[tuple[str, str, int]] = []
    for p in files:
        text = read(p)
        if p.suffix in CODE_EXTS:
            defined.update(OPTICS_NAME_RE.findall(C_COMMENT_RE.sub(" ", text)))
            for c in C_COMMENT_RE.finditer(text):
                for m in OPTICS_NAME_RE.finditer(c.group(0)):
                    mentions.append((p.relative_to(root).as_posix(), m.group(0),
                                     line_of(text, c.start() + m.start())))
        elif p.name == "meson.build":
            # `-DNAME=1` defines a macro; split the flag so NAME is a whole word.
            code = HASH_COMMENT_RE.sub(" ", text).replace("-D", "-D ")
            defined.update(OPTICS_NAME_RE.findall(code))
        elif p.suffix == ".md":
            for m in OPTICS_NAME_RE.finditer(text):
                mentions.append((p.relative_to(root).as_posix(), m.group(0),
                                 line_of(text, m.start())))
    return [
        f"{rel}:{line}: '{name}' is not defined anywhere in the tree (stale or misspelled API name)"
        for rel, name, line in mentions
        if name not in defined and not RUNTIME_NAMES_RE.match(name)
    ]


# A repository path mentioned in a comment, doc or build file must exist,
# relative to the repository root or to the file that names it.
REPO_PATH_RE = re.compile(
    r"(?<![\w./-])((?:libs|tests|examples|tools|crates|docs|include)/[A-Za-z0-9_./-]+"
    r"\.(?:c|h|m|rs|slang|py|build|md|toml|sh|txt|xml|supp|yml))\b"
)
# Paths a tool names precisely so that they must NOT exist.
FORBIDDEN_PATHS = frozenset({"crates/Cargo.toml"})


def check_dangling_paths(root: Path, files: list[Path]) -> list[str]:
    errors: list[str] = []
    for p in files:
        rel = p.relative_to(root).as_posix()
        if is_guardrail_file(rel):
            continue
        text = read(p)
        for m in REPO_PATH_RE.finditer(text):
            ref = m.group(1)
            if ref in FORBIDDEN_PATHS:
                continue
            if (root / ref).exists() or (p.parent / ref).exists():
                continue
            errors.append(f"{rel}:{line_of(text, m.start())}: path '{ref}' does not exist")
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
