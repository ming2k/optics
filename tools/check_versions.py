#!/usr/bin/env python3
"""Version lockstep across every distribution surface.

The C libraries, the umbrella header, Meson's project version, and the Rust
workspace are four independent files that must name the same release. A
runtime accessor test cannot see Meson or Cargo, and a Cargo build cannot see
the C headers. This script reads all four surfaces from the tree and fails on
any disagreement.

Run standalone or through `meson test --suite lint`.
"""

from __future__ import annotations

import re
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MESON_VERSION_RE = re.compile(r"^\s*version\s*:\s*'([0-9]+\.[0-9]+\.[0-9]+)'", re.MULTILINE)
C_VERSION_RE = re.compile(
    r"^#define\s+([A-Z][A-Z0-9_]*)_VERSION_(MAJOR|MINOR|PATCH)\s+([0-9]+)\s*$",
    re.MULTILINE,
)


def parse_meson_version() -> str:
    text = (ROOT / "meson.build").read_text(encoding="utf-8")
    match = MESON_VERSION_RE.search(text)
    if not match:
        raise ValueError("meson.build: no project version triple found")
    return match.group(1)


def parse_cargo_version() -> str:
    with (ROOT / "Cargo.toml").open("rb") as handle:
        manifest = tomllib.load(handle)
    version = manifest.get("workspace", {}).get("package", {}).get("version")
    if not isinstance(version, str):
        raise ValueError("Cargo.toml: missing [workspace.package].version")
    return version


def parse_c_versions() -> dict[str, dict[str, int]]:
    versions: dict[str, dict[str, int]] = {}
    headers = [ROOT / "include" / "optics" / "optics.h"]
    headers += sorted((ROOT / "libs").glob("*/include/**/*.h"))
    for header in headers:
        text = header.read_text(encoding="utf-8")
        for prefix, part, value in C_VERSION_RE.findall(text):
            versions.setdefault(prefix, {})[part] = int(value)
    return versions


def main() -> int:
    errors: list[str] = []

    try:
        meson_version = parse_meson_version()
    except (OSError, ValueError) as exc:
        errors.append(str(exc))
        meson_version = None

    try:
        cargo_version = parse_cargo_version()
    except (OSError, ValueError, tomllib.TOMLDecodeError) as exc:
        errors.append(str(exc))
        cargo_version = None

    if meson_version and cargo_version and meson_version != cargo_version:
        errors.append(f"Cargo.toml workspace version {cargo_version} != meson.build {meson_version}")

    if cargo_version:
        for manifest_path in sorted((ROOT / "bindings" / "crates").glob("*/Cargo.toml")):
            try:
                with manifest_path.open("rb") as handle:
                    manifest = tomllib.load(handle)
            except (OSError, tomllib.TOMLDecodeError) as exc:
                errors.append(f"{manifest_path.relative_to(ROOT)}: {exc}")
                continue
            for section in ("dependencies", "dev-dependencies", "build-dependencies"):
                for name, spec in manifest.get(section, {}).items():
                    if not isinstance(spec, dict) or "path" not in spec:
                        continue
                    version = spec.get("version")
                    if isinstance(version, str) and version != cargo_version:
                        errors.append(
                            f"{manifest_path.relative_to(ROOT)}: dependency {name} pins "
                            f"{version}, workspace is {cargo_version}"
                        )

    versions = parse_c_versions()
    required = ("MAJOR", "MINOR", "PATCH")
    for prefix, parts in sorted(versions.items()):
        missing = [part for part in required if part not in parts]
        if missing:
            errors.append(f"{prefix}_VERSION_{missing[0]} missing (incomplete version triple)")
            continue
        actual = f"{parts['MAJOR']}.{parts['MINOR']}.{parts['PATCH']}"
        if meson_version and actual != meson_version:
            errors.append(f"{prefix}_VERSION_* is {actual}, meson.build is {meson_version}")

    optics = versions.get("OPTICS")
    if optics is None:
        errors.append("include/optics/optics.h: OPTICS_VERSION_* triple missing")
    elif meson_version:
        actual = f"{optics['MAJOR']}.{optics['MINOR']}.{optics['PATCH']}"
        if actual != meson_version:
            errors.append(f"OPTICS_VERSION_* is {actual}, meson.build is {meson_version}")

    if (ROOT / "bindings" / "Cargo.toml").exists():
        errors.append("bindings/Cargo.toml exists: the workspace root must be the repository root")

    if errors:
        sys.stderr.write("version lockstep failed:\n")
        for error in errors:
            sys.stderr.write(f"  - {error}\n")
        return 1

    prefixes = ", ".join(sorted(versions))
    print(f"version lockstep ok: {meson_version} ({prefixes})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
