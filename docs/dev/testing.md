# Testing

## Scope

The root Meson build owns all C tests for `flux`, `lens`, and `iris`.
The Rust bindings are three in-tree Cargo workspaces with their own tests.
Windowed examples remain manual checks because they need a compositor and GPU
surface.

## Test Model

| Channel | Location | Environment |
|---------|----------|-------------|
| `flux` unit tests | `tests/flux/unit/` | CPU-only, except device creation may probe Vulkan. |
| `flux` integration tests | `tests/flux/integration/` | Vulkan 1.3 device or Lavapipe; unsupported capabilities skip cleanly. |
| `flux` benchmarks | `tests/flux/bench/` | CPU timing; registered under the `bench` suite. |
| `lens` tests | `tests/lens/` | Headless and deterministic. |
| `iris` tests | `tests/iris/` | Headless API, theme, version, and accessibility helpers. |
| Rust tests | `bindings/*-rs/` | A configured C build tree or installed C libraries. |
| Interactive checks | `examples/` | Depends on the example; `iris` and most `flux` demos need Wayland/GLFW. |
| Windows TU compile check | `tools/zig-win32-check.sh` | Cross-compiles Windows-only sources with zig cc + MinGW-w64 headers; no Windows host needed. |

On Windows and macOS the same suites run; skip the GPU-less lanes with
`--no-suite integration --no-suite bench --no-suite fuzz` where no Vulkan
driver is present. `canvas_consistency` (integration) is the portability
gate: it compares the GPU backend against the CPU software oracle
pixel-by-pixel and is expected to pass on every platform's Vulkan driver
(MoltenVK >= 1.3 on macOS). See
[Cross-platform](cross-platform.md).

## Run Tests

Enable tests when configuring:

```bash
meson setup build -Dtests=true -Dexamples=true
meson compile -C build
```

Common selections:

```bash
meson test -C build --no-suite bench      # correctness tests
meson test -C build --suite unit          # flux unit suite
meson test -C build --suite integration   # flux GPU suite
meson test -C build test_theme            # one named test
meson test -C build -v test_theme         # verbose single test
meson test -C build --suite bench -v      # benchmarks only
```

`meson test -C build` also runs the benchmarks because they are registered as
tests. Use `--no-suite bench` for the normal correctness gate. Full logs are
written to `build/meson-logs/testlog.txt`.

## Interpret Failures

| Symptom | Inspect first |
|---------|---------------|
| `flux` unit failure | The matching file in `tests/flux/unit/` and its source module. |
| Vulkan test skips | Vulkan 1.3 device discovery, ICD selection, and required extensions. |
| Vulkan validation error | The integration test's frame/resource lifetime and `libs/flux/src/`. |
| `lens` widget or layout failure | The same-named test in `tests/lens/` and `libs/lens/src/`. |
| `iris` theme or API failure | `libs/iris/src/theme_linux.c` or `libs/iris/src/app.c`. |
| Window is blank or examples fail to start | Wayland/GLFW setup and the relevant example; this path is not covered by headless tests. |

## Sanitizers

Use a separate build tree so sanitizer flags do not contaminate the normal
build:

```bash
meson setup build-asan -Dtests=true \
  -Db_sanitize=address,undefined
meson compile -C build-asan
meson test -C build-asan --no-suite bench
```

GPU drivers and system libraries may produce external sanitizer noise. Triage
the first stack frame in project code before changing suppressions.

## Rust Workspaces

Configure and build the C stack first, then point each binding workspace at
the same source and build roots:

```bash
export FLUX_SOURCE_DIR="$PWD/libs/flux"
export LENS_SOURCE_DIR="$PWD"
export IRIS_SOURCE_DIR="$PWD"
export FLUX_BUILD_DIR="$PWD/build"
export LENS_BUILD_DIR="$PWD/build"
export IRIS_BUILD_DIR="$PWD/build"

cargo test --manifest-path bindings/flux-rs/Cargo.toml --workspace
cargo test --manifest-path bindings/lens-rs/Cargo.toml --workspace
cargo test --manifest-path bindings/iris-rs/Cargo.toml --workspace
```

The build scripts use `build/meson-uninstalled/*.pc`, so no local install is
required.

## Add a Test

- Put `flux` CPU-only cases in `tests/flux/unit/` and Vulkan cases in
  `tests/flux/integration/`; register them in the local `meson.build`.
- Use `EXPECT`, `EXPECT_NEAR`, and `TEST_SUMMARY` from
  `tests/flux/test_helpers.h`.
- Put `lens` and `iris` cases in their matching `tests/` directory; use each
  directory's `test_helpers.h` and register the executable in its
  `meson.build`.
- Keep automated `lens` and `iris` tests independent of a live compositor,
  portal, D-Bus session, and physical GPU.
- Add Rust coverage in the workspace that exposes the changed API.

## Manual or Example Verification

Build examples with `-Dexamples=true`, then run the smallest relevant program:

```bash
./build/examples/flux/canvas_hello
./build/examples/lens/headless_demo
./build/examples/iris/hello_app
```

Record the compositor, driver, and GPU when reporting a visual or
platform-specific result.

## See Also

- [Setup](setup.md)
- [Project layout](project-layout.md)
- [Application architecture](../explanation/application-architecture.md)
