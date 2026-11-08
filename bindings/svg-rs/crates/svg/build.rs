//! Re-emit the rpaths published by `svg-sys` (via its `links` metadata) so this
//! crate's own binaries — integration tests and examples — find libsvg *and* the
//! libflux it links at run time, without `LD_LIBRARY_PATH`.
//! `rustc-link-arg` does not propagate across crates, hence this relay.
//!
//! The union is re-published through this crate's `links = "svg_rs"` metadata, so
//! a downstream that depends only on `svg` can relay the same dirs to its own
//! test binaries via `DEP_SVG_RS_RPATHS` without direct `-sys` dependencies.

fn main() {
    let target_os = std::env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    if target_os == "windows" {
        // Windows staging happens in svg-sys; there is nothing to relay.
        println!("cargo:rpaths=");
        return;
    }
    let mut all: Vec<String> = Vec::new();
    for var in ["DEP_SVG_RPATHS", "DEP_FLUX_RPATHS"] {
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
