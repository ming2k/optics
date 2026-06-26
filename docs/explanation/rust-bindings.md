# Rust Bindings

Rust bindings to flux live in the separate **[flux-rs][flux-rs]** repository:

<https://github.com/ming2k/flux-rs>

The split follows the industry convention for Rust bindings to C libraries
(`openssl-sys`, `libsqlite3-sys`, `rust-curl`, `gtk-rs` all live outside
the C source tree). Keeping them out of tree lets the C library and the
bindings follow independent release cadences and stability policies, and
it removes any Rust toolchain requirement from the C build.

## What moved

flux-rs ships the same five crates that previously lived under this
repo's `crates/` directory:

| Crate              | Role                                                          |
|--------------------|---------------------------------------------------------------|
| `flux-sys`         | Raw bindgen FFI to `libflux`. Owns `links = "flux"`.          |
| `flux`             | Safe wrapper: RAII handles, `Result<T, Error>`.              |
| `flux-text-sys`    | Raw bindgen FFI to `libflux-text` (HarfBuzz shaping sibling). |
| `flux-text`        | Safe wrapper over `flux-text-sys`, Layer-0 shaping surface.  |
| `flux-text-layout` | Pure-Rust Layer-1 line wrapping on top of `flux-text`.       |

See the flux-rs README for build integration (`FLUX_SOURCE_DIR`,
`FLUX_BUILD_DIR`, and `FLUX_USE_INSTALLED` semantics).

## Historical reference

The original design rationale for the two-crate split (`flux-sys` raw
FFI + `flux` safe wrapper) is preserved in the flux-rs git history at
the extraction commit. The link-ownership, lint-isolation,
`unsafe`-boundary, and direct-FFI-consumer arguments documented there
still apply within flux-rs.

[flux-rs]: https://github.com/ming2k/flux-rs
