//! Re-emit the rpaths published by `flux-sys` (via its `links` metadata) so that
//! *this* crate's binaries — integration tests and examples — find the flux
//! shared library in the meson build tree at runtime without
//! `LD_LIBRARY_PATH`. `rustc-link-arg` does not propagate across crates,
//! hence this thin relay.

fn main() {
    // Rpaths are a unix concept; on Windows flux-sys stages the DLLs next
    // to the cargo profile output instead, so there is nothing to relay.
    let target_os = std::env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    if target_os == "windows" {
        return;
    }
    if let Ok(rpaths) = std::env::var("DEP_FLUX_RPATHS") {
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
}
