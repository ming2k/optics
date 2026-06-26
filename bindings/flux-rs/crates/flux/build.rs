//! Re-emit the rpaths published by `flux-sys` (via its `links` metadata) so that
//! *this* crate's binaries — integration tests and examples — find libflux.so in
//! the meson build tree at runtime without `LD_LIBRARY_PATH`. `rustc-link-arg`
//! does not propagate across crates, hence this thin relay.

fn main() {
    if let Ok(rpaths) = std::env::var("DEP_FLUX_RPATHS") {
        // Old-style DT_RPATH (not DT_RUNPATH) so the search also covers
        // transitive deps.
        println!("cargo:rustc-link-arg=-Wl,--disable-new-dtags");
        for dir in rpaths.split(';').filter(|s| !s.is_empty()) {
            println!("cargo:rustc-link-arg=-Wl,-rpath,{dir}");
        }
    }
}
