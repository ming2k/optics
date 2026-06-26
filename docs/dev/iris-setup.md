# Setup

How to create and maintain a working development environment for iris.
For the shortest user-facing build, see the
[root README quick start](../../README.md#build); this page covers the
contributor path.

## Prerequisites

iris is the L3 layer of the flux/lens stack. When built with subprojects
(see [Build the whole stack](#build-the-whole-stack-subprojects)), flux and
lens are pulled from source automatically and need **not** be installed
separately. The standalone path
([Build against an installed lens](#build-against-an-installed-lens-standalone))
does require them in `$PREFIX`.

| Tool | Version | Notes |
|------|---------|-------|
| Meson | `>= 1.8` | The Wayland module is stable since 1.8. |
| Ninja | any | Meson's default backend. |
| C compiler | GCC `>= 15` or Clang `>= 19` | C23 (`c2x`) is required. |
| `pkg-config` | any | Locates lens / flux / libsystemd. |
| Python 3 | any | Meson dependency. |

System libraries (Linux/Wayland backend):

| Library | Required | Role |
|---------|----------|------|
| `wayland-client` | yes (Wayland backend) | display, seat, xdg-shell |
| `wayland-protocols` | yes (Wayland backend) | protocol XML, scanned at build time |
| `wayland-scanner` | yes (Wayland backend) | protocol glue generation |
| `xkbcommon` | yes (Wayland backend) | keymap / keyboard state |
| `vulkan` (loader + headers) | yes (Wayland backend) | GPU device + swapchain |
| `wayland-cursor` | optional | themed cursors via `iris_set_cursor`; absent → no-op |
| `libsystemd` | optional | sd-bus live theme watch + AT-SPI bridge; absent → stub translation units |
| `gdbus` + an xdg-desktop-portal backend | runtime only | `iris_pick_file` |
| `gsettings` (GNOME / Cinnamon / MATE / Unity) | runtime only | `iris_query_system_color_scheme`; other DEs fall back to dark |

Debian / Ubuntu one-liner (matches CI):

```sh
sudo apt-get install -y --no-install-recommends \
  build-essential meson ninja-build pkg-config python3 \
  libvulkan-dev libwayland-dev wayland-protocols \
  libxkbcommon-dev libsystemd-dev
```

## Build the whole stack (subprojects)

iris pulls in lens, and lens pulls in flux, as Meson subprojects. A single
command builds the entire `flux → lens → iris` stack from source — **no
inter-repo `meson install`, no `LD_LIBRARY_PATH`**. The subprojects are live
symlinks to the sibling checkouts (`subprojects/lens → ../../lens`,
`lens/subprojects/flux → ../../flux`), so edits in any layer rebuild on the
next `meson compile`.

```sh
# from the iris root, with ../lens and ../flux checked out alongside
meson setup build --wrap-mode=forcefallback -Dexamples=true
meson compile -C build
```

`--wrap-mode=forcefallback` makes Meson use the subprojects even when lens
and flux are also installed in `$PREFIX`. The built binaries resolve
`libflux` / `liblens` / `libiris` from the build tree via `$ORIGIN` rpath,
so examples run directly:

```sh
./build/libs/iris/examples/hello_app
```

This is the contributor default and the way to hack on all three layers at
once. See
[cross-cutting ADR-0001](../adr/0001-meson-subprojects-for-the-stack.md)
for the rationale and the wrap-mode trade-offs.

## Build against an installed lens (standalone)

If lens and flux are already installed in `$PREFIX` (see
[Building flux and lens](#building-flux-and-lens)), iris builds against
them via `pkg-config`, without subprojects:

```sh
export PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig:$PKG_CONFIG_PATH
meson setup build -Dexamples=true
meson compile -C build
```

To install iris itself (so other projects can `pkg-config iris`):

```sh
meson setup --prefix=$PREFIX build -Dexamples=true
meson compile -C build && meson install -C build
```

## Build options

Options come from `meson_options.txt`:

| Option | Default | Meaning |
|--------|---------|---------|
| `-Dexamples=` | `false` | Build the seven windowed C demos under `libs/iris/examples/`. |
| `-Dtests=` | `false` | Build the headless C test suite under `libs/iris/tests/`. |

Auto-detected at configure time (no flag):

| Sentinel | Defined when | Effect when absent |
|----------|---------------|--------------------|
| `IRIS_HAVE_WAYLAND` | wayland-client + xkbcommon + vulkan found | `IRIS_BUILD_NO_BACKEND` defined; `iris_app_run` returns non-zero. |
| `IRIS_HAVE_PORTAL_WATCH` | libsystemd found | `iris_watch_system_color_scheme` returns `-1`; stub compiled. |
| `IRIS_HAVE_ATSPI` | libsystemd found | `iris_a11y_init` returns `-1`; stub compiled. |

To reconfigure after changing options or installing a dependency:

```sh
meson setup --reconfigure build
```

## Building flux and lens

With subprojects (the default above) flux and lens build automatically; skip
this section. For the standalone path, build and install each layer in turn
before iris:

```sh
# 1. flux
git clone https://github.com/ming2k/flux.git ../flux
meson setup --prefix=$PREFIX ../flux/build ../flux
meson compile -C ../flux/build && meson install -C ../flux/build

# 2. lens (needs flux via pkg-config)
git clone https://github.com/ming2k/lens.git ../lens
PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig meson setup --prefix=$PREFIX ../lens/build ../lens -Dtests=true
PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig meson compile -C ../lens/build
PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig meson install -C ../lens/build
```

## Rust bindings

The Rust bindings (`iris-sys`, `iris`) live in the separate
[`iris-rs`](https://github.com/ming2k/iris-rs) repository. Build the C
libraries (flux → lens → iris) first, then see the iris-rs README.

## Development options

- `-Dexamples=true -Dtests=true` — turn on for day-to-day work.
- `--wrap-mode=forcefallback` — force flux/lens subprojects even when
  installed in `$PREFIX` (the contributor default for cross-layer work).
- A build without libsystemd exercises the stub translation units
  (`theme_watch_stub.c`, `a11y_stub.c`); the CI `stubs` job covers this.
- `IRIS_BUILD_NO_BACKEND` builds a linkable `libiris` whose
  `iris_app_run` returns non-zero — for header / bindgen / platform-less
  CI hosts.

## Troubleshooting

| Symptom | First check |
|---------|-------------|
| `Dependency "lens" not found` | Built via subprojects (`--wrap-mode=forcefallback`)? Otherwise flux then lens installed into `$PREFIX` and `PKG_CONFIG_PATH` includes `$PREFIX/lib/pkgconfig`? |
| `meson_version >= 1.8` error | Upgrade Meson (`pip install --user meson`). |
| Theme live-watch / a11y no-op | `libsystemd` installed? Check the Meson configure log for the `libsystemd not found` message. |
| `iris_set_cursor` does nothing | Wayland backend compiled in? (Look for `IRIS_HAVE_WAYLAND`.) The compositor must also offer `wp_cursor_shape_manager_v1`. |
| `iris_pick_file` fails at runtime | A working `gdbus` and an xdg-desktop-portal backend must be present on the desktop. |

## See also

- [testing.md](testing.md) — running and extending the test suite.
- [project-layout.md](project-layout.md) — where new files go.
- [Cross-cutting ADR-0001](../adr/0001-meson-subprojects-for-the-stack.md) — the subproject decision.
- [Root README](../../README.md#build) — the short user-facing build.
- [CI workflow](../../.github/workflows/ci.yml) — the canonical flux → lens → iris build order.
