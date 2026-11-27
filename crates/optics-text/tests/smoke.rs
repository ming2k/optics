//! External-crate smoke test for `optics-text` public re-exports.

#![deny(rust_2018_idioms)]

#[test]
fn reexported_gfx_is_reachable() {
    assert!(!optics_text::gfx::version().is_empty());
}
