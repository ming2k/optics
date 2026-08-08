//! Re-emit the rpaths published by `flux-text-sys` (via its `links` metadata)
//! so that *this* crate's binaries — unit tests — find the flux-text (and
//! flux, which lives in the same build dir) shared libraries without
//! LD_LIBRARY_PATH. `rustc-link-arg` does not propagate across crates,
//! hence this relay.

fn main() {
    // Rpaths are a unix concept; on Windows flux-text-sys stages the DLLs
    // next to the cargo profile output instead, so there is nothing to relay.
    let target_os = std::env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    if target_os == "windows" {
        return;
    }
    if let Ok(rpaths) = std::env::var("DEP_FLUX_TEXT_RPATHS") {
        // --disable-new-dtags: GNU-style ELF linkers only (see the flux
        // crate's build.rs); macOS ld64 rejects it.
        if target_os != "macos" {
            println!("cargo:rustc-link-arg=-Wl,--disable-new-dtags");
        }
        for dir in rpaths.split(';').filter(|s| !s.is_empty()) {
            println!("cargo:rustc-link-arg=-Wl,-rpath,{dir}");
        }
    }
}
