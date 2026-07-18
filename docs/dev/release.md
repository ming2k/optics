# Release

Optics currently uses a manual source-release process. There is no root
changelog or repository release workflow, so release notes and publication
remain explicit maintainer steps.

## Version Domains

The repository contains related but distinct versions:

| Surface | Source of version |
|---------|-------------------|
| Root Meson distribution, C shared objects, and `.pc` files | `project(version:)` in `meson.build` |
| C API compile-time/runtime version helpers | `*_VERSION_MAJOR`, `*_MINOR`, and `*_PATCH` in public headers |
| Rust crates | `[workspace.package].version` in each `bindings/*-rs/Cargo.toml` |

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
meson test -C build-release --no-suite bench

meson setup build-release-asan -Dtests=true \
  -Db_sanitize=address,undefined
meson compile -C build-release-asan
meson test -C build-release-asan --no-suite bench
```

Point the binding build scripts at `build-release` and run all three Cargo
workspaces as described in [Testing](testing.md#rust-workspaces). Manually run
at least one `flux` windowed example and one `iris` example on Wayland.

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
their manifests and metadata.

## Post-release Checks

- The tag resolves to the tested commit.
- A fresh checkout of the tag passes the release build.
- The source archive checksum matches the published artifact.
- `pkg-config --modversion flux lens iris` reports the intended installed
  metadata after installing into a temporary prefix.
- Runtime C version helpers and published Rust crate versions match the release
  notes.
