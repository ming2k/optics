//! Re-emit the rpaths published by `iris-sys` (via its `links = "iris"`
//! metadata) so that *this* crate's targets — examples, unit-test harnesses,
//! integration tests — find the iris (+ lens/flux) shared libraries at
//! runtime without `LD_LIBRARY_PATH`. Publish the same normalized list
//! through this crate's `links = "iris_rs"` metadata so direct downstream
//! crates can relay it from `DEP_IRIS_RS_RPATHS` for their own test/example
//! binaries.
//!
//! `rustc-link-arg` does not propagate across crates, so `iris-sys` cannot
//! add the rpath itself; it publishes the dirs (`cargo:rpaths`) and we relay
//! them here. This mirrors the `flux` crate's build.rs (DEP_FLUX_RPATHS).

fn main() {
    let rpaths = std::env::var("DEP_IRIS_RPATHS").unwrap_or_default();
    let dirs: Vec<&str> = rpaths.split(';').filter(|s| !s.is_empty()).collect();
    // Rpaths are a unix concept; on Windows iris-sys stages the DLLs next
    // to the cargo profile output instead, so there is nothing to link-arg.
    let target_os = std::env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    if !dirs.is_empty() && target_os != "windows" {
        // Old-style DT_RPATH (not DT_RUNPATH) so the search also covers
        // transitive deps (liblens, libflux). GNU-style ELF linkers only:
        // macOS ld64 has no DT_RUNPATH distinction and rejects
        // --disable-new-dtags.
        if target_os != "macos" {
            println!("cargo:rustc-link-arg=-Wl,--disable-new-dtags");
        }
        for dir in &dirs {
            println!("cargo:rustc-link-arg=-Wl,-rpath,{dir}");
        }
    }
    println!("cargo:rpaths={}", dirs.join(";"));
}
