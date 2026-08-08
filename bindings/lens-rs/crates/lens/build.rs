//! Re-emit the rpaths published by `lens-sys` (via its `links` metadata) so
//! that *this* crate's binaries — integration tests and examples — find the
//! lens (and flux, which lives in flux's build dir) shared libraries in the
//! meson build tree at runtime without `LD_LIBRARY_PATH`. `rustc-link-arg`
//! does not propagate across crates, hence this thin relay. Mirrors the
//! `flux` crate's build.rs.

fn main() {
    // Rpaths are a unix concept; on Windows lens-sys stages the DLLs next
    // to the cargo profile output instead, so there is nothing to relay.
    let target_os = std::env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    if target_os == "windows" {
        return;
    }
    // --disable-new-dtags: GNU-style ELF linkers only (see the flux crate's
    // build.rs); macOS ld64 rejects it.
    if target_os != "macos" {
        println!("cargo:rustc-link-arg=-Wl,--disable-new-dtags");
    }
    for var in ["DEP_LENS_RPATHS", "DEP_FLUX_RPATHS"] {
        if let Ok(rpaths) = std::env::var(var) {
            for dir in rpaths.split(';').filter(|s| !s.is_empty()) {
                println!("cargo:rustc-link-arg=-Wl,-rpath,{dir}");
            }
        }
    }
}
