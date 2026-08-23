//! Re-emit the rpaths published by `flux-sys` (via its `links` metadata) so that
//! *this* crate's binaries — integration tests and examples — find the flux
//! shared library in the meson build tree at runtime without
//! `LD_LIBRARY_PATH`. `rustc-link-arg` does not propagate across crates,
//! hence this thin relay.
//!
//! This crate also *re-publishes* the list through its own `links = "flux_rs"`
//! metadata, so a downstream that depends on `flux` (and nothing else from
//! the stack) can relay the same rpaths to its own test/example binaries via
//! `DEP_FLUX_RS_RPATHS` — no direct `flux-sys` dependency required, no
//! hand-maintained relay of its own.

fn main() {
    // Rpaths are a unix concept; on Windows flux-sys stages the DLLs next
    // to the cargo profile output instead, so there is nothing to relay.
    let target_os = std::env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    if target_os == "windows" {
        println!("cargo:rpaths=");
        return;
    }
    let rpaths = std::env::var("DEP_FLUX_RPATHS").unwrap_or_default();
    if !rpaths.is_empty() {
        // Old-style DT_RPATH (not DT_RUNPATH) so the search also covers
        // transitive deps. GNU-style ELF linkers only: macOS ld64 has no
        // DT_RPATH/DT_RUNPATH distinction and rejects --disable-new-dtags.
        if target_os != "macos" {
            println!("cargo:rustc-link-arg=-Wl,--disable-new-dtags");
        }
        for dir in rpaths.split(';').filter(|s| !s.is_empty()) {
            println!("cargo:rustc-link-arg=-Wl,-rpath,{dir}");
        }
    }
    println!("cargo:rpaths={rpaths}");
}
