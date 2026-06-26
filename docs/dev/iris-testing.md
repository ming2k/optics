# Testing

How to run, interpret, and extend the iris test suite.

## Scope

This page covers the automated C test suite in `libs/iris/tests/`. It
does **not** cover the windowed demos — those own a Wayland session and
GPU surface and are exercised by running them, not by `meson test`. See
[Manual verification](#manual-or-example-verification). The Rust test
suite lives with the bindings in the separate
[`iris-rs`](https://github.com/ming2k/iris-rs) repository.

## Test model

Two channels:

| Channel | Harness | Needs a session? |
|---------|---------|------------------|
| `meson test` | `libs/iris/tests/test_helpers.h` (`CHECK` / `CHECK_STR_EQ` / `TEST_REPORT`) | no |
| Examples | none (visual / interactive) | yes (Wayland + GPU) |

The C harness mirrors lens's `libs/lens/tests/test_helpers.h` so the whole
flux/lens/iris stack shares one test style: `CHECK` accumulates failures
and `TEST_REPORT()` prints a summary and returns the process exit code.

Every automated case runs without a Wayland compositor or GPU surface.
The cases that touch the live library call entry points that either do no
I/O (`iris_version_string`) or degrade gracefully when desktop
integration is absent (the `gsettings` / portal probes miss and the safe
default is returned). `iris_app_run` is intentionally never exercised by
the automated suite — it owns a window and event loop.

## Run tests

C suite (requires `-Dtests=true` at configure time):

```sh
meson setup build -Dtests=true
meson compile -C build
meson test -C build                 # full suite
meson test -C build test_theme      # single test
meson test -C build -v              # verbose
```

On failure, Meson writes the full log to
`build/meson-logs/testlog.txt`:

```sh
cat build/meson-logs/testlog.txt
```

## Coverage

| Test | Covers | When to run |
|------|--------|-------------|
| `test_version` | `iris_version_string` / `IRIS_VERSION_*` macros | always |
| `test_app_api` | `iris_app_run` / `iris_pick_file` argument validation (NULL cfg, NULL build+paint, …) | always |
| `test_theme` | colour-scheme query (`GTK_THEME` branch; `gsettings` shadowed) | always |
| `test_a11y_util` | AT-SPI role / value / char-count helpers in `src/a11y_util.c` | always (compiled unconditionally on both CI jobs) |
| `hello_app`, `minimal`, `widgets`, `fonts`, `panels`, `overlay_demo`, `forms`, `desktop_demo` | the windowed path — build callbacks, paint, IME, cursor, theme | manual (needs a session) |

`test_a11y_util` compiles `src/a11y_util.c` directly because the helpers
are internal / hidden-visibility and are not exported from `libiris.so`
(see `libs/iris/tests/meson.build`).

## Interpret failures

| Symptom | Likely subsystem |
|---------|------------------|
| `test_version` mismatch | `IRIS_VERSION_*` macros in `app.h` vs `meson.build` `version:`. |
| `test_app_api` failure | argument validation in `src/app.c` (the dispatcher). |
| `test_theme` failure | `src/theme_linux.c` (`gsettings` / `GTK_THEME` probes). |
| `test_a11y_util` failure | pure helpers in `src/a11y_util.{c,h}`. |
| Examples crash / blank window | Wayland backend (`src/app_wayland.c`), swapchain, or Vulkan init — not the tested surface. |

## Sanitizers

Sanitizers are not wired into the default build. To run the C suite under
AddressSanitizer / UndefinedBehaviorSanitizer:

```sh
meson setup build -Dtests=true -Db_sanitize=address,undefined
meson compile -C build
meson test -C build
```

## Add a test

C test:

1. Add `libs/iris/tests/test_<name>.c`. Include `test_helpers.h`; use
   `CHECK(cond)` / `CHECK_STR_EQ(actual, expected)` and
   `return TEST_REPORT();` from `main`.
2. Register it in `libs/iris/tests/meson.build`. If the test exercises
   internal (hidden-visibility) helpers, compile the source file directly
   next to the test (the `test_a11y_util` pattern) rather than relying on
   the exported symbol.
3. Keep it headless and deterministic: no Wayland, no GPU, no portal, no
   real D-Bus. Probe entry points that degrade gracefully when the desktop
   integration is missing.

Naming: `test_<area>` for C.

## Manual or example verification

The windowed path is verified by the C examples under
`libs/iris/examples/` (built with `-Dexamples=true`):

```sh
meson setup build -Dexamples=true && meson compile -C build
./build/libs/iris/examples/hello_app
./build/libs/iris/examples/overlay_demo   # exercises the paint callback
./build/libs/iris/examples/forms          # exercises IME + cursor
./build/libs/iris/examples/desktop_demo   # menubar, context menu, split, table, modal (ADR-0016..0019)
```

AT-SPI behaviour is verified live on the D-Bus bus with `gdbus` or
`orca`; see the [a11y ADR](../../libs/iris/docs/adr/0004-atspi-bridge-design.md)
for the verification recipe.

## See also

- [setup.md](setup.md) — getting the suite to build.
- [project-layout.md](project-layout.md) — where tests live.
- [CI workflow](../../.github/workflows/ci.yml) — the `test` and `stubs` jobs.
- [API reference](../../libs/iris/docs/reference/api.md) — what each entry point promises.
