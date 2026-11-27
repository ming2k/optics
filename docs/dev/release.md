# Release

Optics uses a manual source-release process. There is no automated repository
release workflow, so cutting release notes and publishing remain explicit
maintainer steps. User-visible changes are recorded by hand in the root
[`CHANGELOG.md`](../../CHANGELOG.md) under `## [Unreleased]`; promoting that
section to a version heading is part of the release commit.

## Version Domains

The repository contains related but distinct versions:

| Surface | Source of version |
|---------|-------------------|
| Root Meson distribution, C shared objects, and `.pc` files | `project(version:)` in `meson.build` |
| C API compile-time/runtime version helpers | `*_VERSION_MAJOR`, `*_MINOR`, and `*_PATCH` in public headers |
| Rust crates | `[workspace.package].version` in the repository-root `Cargo.toml` |

Choose which surfaces are part of the release and update them intentionally.
Do not assume changing the root Meson version updates public header macros or
Cargo package versions.

## Preconditions

- Work from a clean `main` checkout with all intended changes present.
- Decide the release version and prepare release notes from user-visible
  changes.
- Confirm public C headers, runtime version helpers, `.pc` metadata, and Rust
  crate versions agree wherever the release promises them to agree.
- Run the C and Rust gates below on the exact commit to be tagged.

## Build and Test Gates

Use clean release build directories:

```bash
meson setup build-release --buildtype=release \
  -Dexamples=true -Dtests=true
meson compile -C build-release
meson test -C build-release --no-suite bench --no-suite fuzz

meson setup build-release-asan -Dtests=true \
  -Db_sanitize=address,undefined
meson compile -C build-release-asan
meson test -C build-release-asan --no-suite bench --no-suite fuzz
```

Point the binding build scripts at `build-release` and run the repository-root
Cargo workspace as described in [Testing](testing.md#rust-workspaces). Verify
that `meson install -C build-release --destdir=<stage>` produces a self-contained
prefix whose `optics.pc` validates via `pkg-config --validate` and compiles C23
and C++23 umbrella consumers. Follow the
[Pre-release smoke verification checklist](manual-testing.md#6-pre-release-smoke-verification-checklist)
to manually verify interactive `gfx`, `ui`, and `window` applications.

## Create the Source Artifact

Meson can produce the source archive after the release build passes:

```bash
meson dist -C build-release
```

Inspect the archive under `build-release/meson-dist/`, unpack it into a
temporary directory, and repeat the release build from that archive before
publishing it.

## Tag and Publish

```bash
git tag -a v<VERSION> -m "Optics <VERSION>"
git push origin main v<VERSION>
```

Create the repository release from that tag, attach the verified Meson archive
and checksum, and publish the prepared release notes. Publishing Rust crates or
system packages is a separate action and must use the versions recorded in
their manifests and metadata. The Rust crates remain Git/path-distributed until
the publication gate in [Rust Bindings](../explanation/rust-bindings.md#publication-gate)
is met; do not publish a `-sys` crate that still requires `libclang` on the
consumer's build host.

## Post-release Checks

- The tag resolves to the tested commit.
- A fresh checkout of the tag passes the release build.
- The source archive checksum matches the published artifact.
- `pkg-config --modversion optics-gfx optics-ui optics-window` reports the intended
  installed metadata after installing into a temporary prefix.
- Runtime C version helpers and published Rust crate versions match the release
  notes.
