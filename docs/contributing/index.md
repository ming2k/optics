# Contributing to lens

lens is a **UI engine**: a C23 immediate-mode façade over a retained-mode
core, drawing only through [flux](https://github.com/ming2k/flux)'s
canvas. The higher layer, [iris](https://github.com/ming2k/iris),
turns lens into a complete application toolkit (windows, event loop,
a11y, system integration).

This page covers the cross-cutting workflow. The library-specific docs
— architecture, ADRs, and API reference — live co-located with the
code at [libs/lens/docs/](../../libs/lens/docs/index.md).

## Dependency direction

lens is one library (`liblens`); its Rust FFI bindings live in the
separate [`lens-rs`](https://github.com/ming2k/lens-rs) repository.
The dependency graph crosses repository boundaries, and each layer is
built and installed separately:

```text
iris ──▶ lens ──▶ flux
  windows        widget tree     Vulkan canvas
  event loop     layout          text
  a11y bridge    hit-testing
  theme follow   input model
  file dialog
```

lens's defining contract is "input arrives as data, no windowing
linked". Windowing, the event loop, the a11y bus, system theme
following, and file picking all live in iris, not lens — putting them
in lens would break the headless-testability and embeddability that
make lens valuable. Don't push application or windowing concerns down
into lens.

## Building the stack

Build flux first, then lens. (iris is only needed if you are working
on the application toolkit; it lives in a separate repo.) The
[root README](../../README.md#build) has the canonical sequence; in
short:

```sh
# 1. flux
cd ../flux && meson setup --prefix=$PREFIX build \
  && meson compile -C build && meson install -C build

# 2. lens
export PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig:$PKG_CONFIG_PATH
cd ../lens && meson setup build -Dtests=true -Dexamples=true \
  && meson compile -C build && meson test -C build
```

Rust bindings are in the separate
[`lens-rs`](https://github.com/ming2k/lens-rs) repository; see its
README for build instructions.

## Cross-cutting changes are atomic

A change that touches lens *and* flux, or lens *and* iris, is one
commit / one PR per repository, applied bottom-up. Change the lower
layer first, install it, then change the consumer in the same change
set; CI builds the whole stack in dependency order.

## Releases & versioning

Each repository (`flux`, `lens`, `iris`) has its own project version
and its own release tag, for example `lens-v0.1.0`. The layers are
independently consumable ABI/package units and ship on their own
cadence; the split is deliberate and mirrors GSK/GTK and Flutter
Engine/Framework.

## Documentation

Documentation ships in the **same commit** as the code it describes.
Follow the project-neutral [documentation governance](documentation/index.md)
for routing, writing style, the ADR workflow, and the update checklist.
Note its rule: assistants may read and suggest changes to files under
`docs/contributing/documentation/` but must not modify them directly.
