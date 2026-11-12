//! Re-emit the rpaths published by `lens-sys` (via its `links` metadata) so
//! that *this* crate's binaries — integration tests and examples — find the
//! lens (and flux, which lives in flux's build dir) shared libraries in the
//! meson build tree at runtime without `LD_LIBRARY_PATH`. `rustc-link-arg`
//! does not propagate across crates, hence this thin relay. Mirrors the
//! `flux` crate's build.rs.
//!
//! This crate also *re-publishes* the union through its own
//! `links = "lens_rs"` metadata, so a downstream that depends on `lens`
//! (and nothing else from the stack) can relay the same rpaths to its own
//! test/example binaries via `DEP_LENS_RS_RPATHS` — no direct `lens-sys`
//! / `flux-sys` dependencies required.

fn main() {
    // Rpaths are a unix concept; on Windows lens-sys stages the DLLs next
    // to the cargo profile output instead, so there is nothing to relay.
    let target_os = std::env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    if target_os == "windows" {
        println!("cargo:rpaths=");
        return;
    }
    // Union of both sources, de-duplicated in order. lens pulls in flux
    // transitively (meson Requires), so DEP_LENS_RPATHS usually already
    // contains both dirs; the union keeps it correct when only
    // DEP_FLUX_RPATHS carries one.
    let mut all: Vec<String> = Vec::new();
    for var in ["DEP_LENS_RPATHS", "DEP_FLUX_RPATHS"] {
        if let Ok(rpaths) = std::env::var(var) {
            for dir in rpaths.split(';').filter(|s| !s.is_empty()) {
                let dir = dir.to_string();
                if !all.contains(&dir) {
                    all.push(dir);
                }
            }
        }
    }
    if !all.is_empty() {
        // --disable-new-dtags: GNU-style ELF linkers only (see the flux crate's
        // build.rs); macOS ld64 rejects it.
        if target_os != "macos" {
            println!("cargo:rustc-link-arg=-Wl,--disable-new-dtags");
        }
        for dir in &all {
            println!("cargo:rustc-link-arg=-Wl,-rpath,{dir}");
        }
    }
    println!("cargo:rpaths={}", all.join(";"));
}
