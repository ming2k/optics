#!/usr/bin/env python3
"""Monorepo version lockstep guardrail.

Assures that all sibling libraries and language bindings share the exact project
version declared in meson.build. Prevents version macro drift and hard-coded literals.
"""

import glob
import re
import sys
from pathlib import Path

HEADER_SPECS = [
    ("libs/flux/include/flux/core.h", "FLUX", "flux"),
    ("libs/lens/include/lens/lens.h", "LENS", "lens"),
    ("libs/iris/include/iris/app.h", "IRIS", "iris"),
    ("libs/prism/include/prism/types.h", "PRISM", "prism"),
    ("libs/transit/include/transit/transit.h", "TRANSIT", "transit"),
    ("libs/glyph/include/glyph/glyph.h", "GLYPH", "glyph"),
    ("libs/vista/include/vista/vista.h", "VISTA", "vista"),
]


def get_project_version(repo_root: Path) -> tuple[int, int, int]:
    meson_path = repo_root / "meson.build"
    if not meson_path.is_file():
        raise RuntimeError("meson.build not found in repo root")

    content = meson_path.read_text(encoding="utf-8")
    m = re.search(r"version\s*:\s*'([0-9]+)\.([0-9]+)\.([0-9]+)'", content)
    if not m:
        raise RuntimeError("Failed to parse project version from meson.build")

    return int(m.group(1)), int(m.group(2)), int(m.group(3))


def check_lockstep(repo_root: Path) -> list[str]:
    errors = []
    try:
        major, minor, patch = get_project_version(repo_root)
    except Exception as e:
        return [str(e)]

    expected_version = f"{major}.{minor}.{patch}"

    # 1. Check C header version macros
    for rel_path, prefix, name in HEADER_SPECS:
        header_path = repo_root / rel_path
        if not header_path.is_file():
            errors.append(f"FAIL {name}: header {rel_path} missing")
            continue

        content = header_path.read_text(encoding="utf-8")
        maj_m = re.findall(rf"^#define {prefix}_VERSION_MAJOR\s+([0-9]+)", content, re.MULTILINE)
        min_m = re.findall(rf"^#define {prefix}_VERSION_MINOR\s+([0-9]+)", content, re.MULTILINE)
        pat_m = re.findall(rf"^#define {prefix}_VERSION_PATCH\s+([0-9]+)", content, re.MULTILINE)

        if not (maj_m and min_m and pat_m):
            errors.append(f"FAIL {name}: version macros missing in {rel_path}")
            continue

        h_maj, h_min, h_pat = int(maj_m[-1]), int(min_m[-1]), int(pat_m[-1])
        if (h_maj, h_min, h_pat) != (major, minor, patch):
            errors.append(
                f"FAIL {name}: {h_maj}.{h_min}.{h_pat} != project {expected_version} ({rel_path})"
            )

    # 2. Check for hard-coded version literals in runtime macros
    # The runtime version strings must derive from macros, never a literal.
    for p in repo_root.glob("libs/**/src/**/*.c"):
        if "vendor" in p.parts:
            continue
        try:
            c = p.read_text(encoding="utf-8", errors="ignore")
        except Exception:
            continue
        for line_no, line in enumerate(c.splitlines(), start=1):
            if re.search(r'_VERSION_STRING\s+"[0-9]', line):
                rel = p.relative_to(repo_root).as_posix()
                errors.append(f"FAIL: hard-coded version literal found in {rel}:{line_no}")

    # 3. Check Rust bindings workspace versions
    for cargo_path in repo_root.glob("bindings/*-rs/Cargo.toml"):
        content = cargo_path.read_text(encoding="utf-8")
        m = re.search(r'^version\s*=\s*"([^"]+)"', content, re.MULTILINE)
        if m:
            ws_ver = m.group(1)
            if ws_ver != expected_version:
                rel = cargo_path.relative_to(repo_root).as_posix()
                errors.append(f"FAIL {rel}: workspace version '{ws_ver}' != project {expected_version}")

    # 4. Check inter-crate internal dependencies
    dep_re = re.compile(r'path\s*=\s*"[^"]+".*version\s*=\s*"([0-9][^"]*)"')
    for dep_cargo in repo_root.glob("bindings/*-rs/crates/*/Cargo.toml"):
        content = dep_cargo.read_text(encoding="utf-8")
        rel = dep_cargo.relative_to(repo_root).as_posix()
        for line_no, line in enumerate(content.splitlines(), start=1):
            m = dep_re.search(line)
            if m and m.group(1) != expected_version:
                errors.append(f"FAIL {rel}:{line_no}: stale inter-crate version dependency ({m.group(1)} != {expected_version})")

    return errors


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent.parent
    errors = check_lockstep(repo_root)
    if errors:
        sys.stderr.write("\n".join(errors) + "\n\n")
        sys.stderr.write(
            "Version lockstep broken. Bump every library's *_VERSION_* macros and "
            "bindings Cargo.tomls to match meson.build (one release, one version).\n"
        )
        return 1

    major, minor, patch = get_project_version(repo_root)
    print(f"version lockstep OK (all libraries and bindings at {major}.{minor}.{patch})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
