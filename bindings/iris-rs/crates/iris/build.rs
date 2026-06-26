//! Re-emit the rpaths published by `iris-sys` (via its `links = "iris"`
//! metadata) so that *this* crate's targets — examples, unit-test harnesses,
//! integration tests — find libiris.so (+ liblens/libflux) at runtime
//! without `LD_LIBRARY_PATH`.
//!
//! `rustc-link-arg` does not propagate across crates, so `iris-sys` cannot
//! add the rpath itself; it publishes the dirs (`cargo:rpaths`) and we relay
//! them here. This mirrors the `flux` crate's build.rs (DEP_FLUX_RPATHS).

fn main() {
    if let Ok(rpaths) = std::env::var("DEP_IRIS_RPATHS") {
        let dirs: Vec<&str> = rpaths.split(';').filter(|s| !s.is_empty()).collect();
        if !dirs.is_empty() {
            // Old-style DT_RPATH (not DT_RUNPATH) so the search also covers
            // transitive deps (liblens, libflux).
            println!("cargo:rustc-link-arg=-Wl,--disable-new-dtags");
            for dir in &dirs {
                println!("cargo:rustc-link-arg=-Wl,-rpath,{dir}");
            }
        }
    }
}
