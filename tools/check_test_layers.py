#!/usr/bin/env python3
"""Test-architecture guardrail.

The test pyramid is not a document that everyone is trusted to remember. This
script reads Meson's test introspection and fails when a test escapes the
taxonomy, when a layer is renamed, or when the E2E directory starts depending on
private library internals.

Run it directly:

    python3 tools/check_test_layers.py build

or through Meson (`meson test --suite lint`).
"""

from __future__ import annotations

import json
import pathlib
import subprocess
import sys

LAYERS = {"unit", "integration", "e2e"}
LANES = {"bench", "fuzz"}
META = {"lint", "tooling"}
ALLOWED = LAYERS | LANES | META


def suite_names(entry: dict) -> set[str]:
    names = set()
    for suite in entry.get("suite", []):
        # Meson prefixes every suite with the project name: "optics:unit".
        names.add(suite.split(":", 1)[-1])
    return names


def check_introspection(build_dir: pathlib.Path) -> list[str]:
    errors: list[str] = []
    result = subprocess.run(
        ["meson", "introspect", str(build_dir), "--tests"],
        check=True,
        capture_output=True,
        text=True,
    )
    tests = json.loads(result.stdout)

    seen: dict[str, int] = {}
    layers: dict[str, int] = {name: 0 for name in sorted(LAYERS)}
    for entry in tests:
        name = entry["name"]
        seen[name] = seen.get(name, 0) + 1
        suites = suite_names(entry)
        unknown = suites - ALLOWED
        if unknown:
            errors.append(f"{name}: unknown suite(s) {sorted(unknown)}")

        declared_layers = suites & LAYERS
        declared_lanes = suites & LANES
        if len(declared_layers) > 1:
            errors.append(f"{name}: declares multiple layers {sorted(declared_layers)}")
        if declared_layers and declared_lanes:
            errors.append(f"{name}: mixes a layer {sorted(declared_layers)} with lane {sorted(declared_lanes)}")
        if not declared_layers and not declared_lanes and not (suites & META):
            errors.append(
                f"{name}: no unit/integration/e2e layer, bench/fuzz lane, or lint/tooling classification"
            )
        for layer in declared_layers:
            layers[layer] += 1

    for name, count in sorted(seen.items()):
        if count > 1:
            errors.append(f"{name}: registered {count} times; test names are globally unique")

    if layers["unit"] == 0:
        errors.append("no unit tests: the pyramid has no base")
    if layers["integration"] == 0:
        errors.append("no integration tests: the boundary layer is unguarded")
    if layers["e2e"] == 0:
        errors.append("no e2e tests: the public-api path is unguarded")
    if layers["unit"] < layers["e2e"]:
        errors.append("more e2e tests than unit tests: the pyramid is inverted")

    print(
        "test layers: "
        + ", ".join(f"{name}={count}" for name, count in sorted(layers.items()))
    )
    return errors


def check_no_private_harness(root: pathlib.Path) -> list[str]:
    errors = []
    for path in sorted(root.glob("tests/**/test_helpers.h")):
        errors.append(f"{path}: private assertion harness; use tests/support/optics_test.h")
    for path in sorted(root.glob("tests/**/test_helpers.c")):
        errors.append(f"{path}: private harness implementation; use tests/support/optics_test.c")

    # A renamed private harness is still a private harness. The shared header
    # is the only place these identifiers may be defined.
    support = root / "tests" / "support"
    legacy_tokens = ("TEST_SUMMARY", "TEST_REPORT", "#define CHECK(", "#define EXPECT(",
                     "#define OT_EXPECT(")
    for pattern in ("tests/**/*.c", "tests/**/*.h"):
        for path in sorted(root.glob(pattern)):
            if support in path.parents:
                continue
            text = path.read_text()
            for token in legacy_tokens:
                if token in text:
                    errors.append(
                        f"{path}: legacy/private assertion token {token!r}; use optics_test.h"
                    )
    return errors


def check_unit_cpu_purity(root: pathlib.Path) -> list[str]:
    """Unit suites must not create a Vulkan device.

    The layer contract is executable: a unit test that reaches for a device is
    an integration test with the wrong label. Board-level probing is excluded
    by token, not by intention.
    """
    errors = []
    forbidden = ("flux_device_create", "flux_test_make_", "vkCreate", "vkAllocate",
                 "VkDevice", "VkPhysicalDevice", "VkInstance")
    candidates = list(root.glob("tests/**/unit/**/*.c"))
    # These directories are unit-shaped at the top level; their integration/
    # and fuzz/ subdirectories are allowed to touch a device.
    for library in ("lens", "iris", "reticle", "transit", "vista"):
        for path in (root / "tests" / library).glob("*.c"):
            candidates.append(path)
    for path in sorted(candidates):
        text = path.read_text()
        for token in forbidden:
            if token in text:
                errors.append(f"{path}: unit test uses {token!r}; move it to integration")
    return errors


def check_e2e_registration(root: pathlib.Path) -> list[str]:
    errors = []
    e2e = root / "tests" / "e2e"
    meson = e2e / "meson.build"
    if not meson.is_file():
        return ["tests/e2e/meson.build is missing"]
    text = meson.read_text()
    if "optics_dep" not in text:
        errors.append("tests/e2e/meson.build must link the public optics_dep façade")
    for path in sorted(e2e.glob("*.c")):
        if path.name not in text:
            errors.append(f"{path}: E2E source is not registered in tests/e2e/meson.build")
    return errors


def check_e2e_boundary(root: pathlib.Path) -> list[str]:
    errors: list[str] = []
    e2e = root / "tests" / "e2e"
    if not e2e.is_dir():
        return ["tests/e2e is missing"]

    forbidden_include = ("internal.h", "libs/", "<libs/")
    for path in sorted(e2e.glob("*.c")):
        for number, line in enumerate(path.read_text().splitlines(), 1):
            stripped = line.strip()
            if not stripped.startswith("#include"):
                continue
            if any(token in stripped for token in forbidden_include):
                errors.append(f"{path}:{number}: e2e includes a private header: {stripped}")

    meson = e2e / "meson.build"
    if meson.is_file():
        text = meson.read_text()
        if "../../libs/" in text:
            errors.append(
                "tests/e2e/meson.build compiles library sources; e2e must link public targets"
            )
    return errors


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: check_test_layers.py <build-dir>", file=sys.stderr)
        return 2
    root = pathlib.Path(__file__).resolve().parent.parent
    build_dir = pathlib.Path(sys.argv[1]).resolve()

    errors = []
    errors.extend(check_introspection(build_dir))
    errors.extend(check_no_private_harness(root))
    errors.extend(check_unit_cpu_purity(root))
    errors.extend(check_e2e_boundary(root))
    errors.extend(check_e2e_registration(root))

    if errors:
        print("test layering violations:", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1
    print("test layering: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
