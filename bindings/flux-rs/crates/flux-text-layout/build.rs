//! Re-emit the rpaths published by `flux-text-sys` and `flux-sys` (via their
//! `links` metadata) so that *this* crate's binaries — the wrap unit tests —
//! find libflux_text.so and libflux.so in the meson build tree at runtime
//! without `LD_LIBRARY_PATH`. `rustc-link-arg` does not propagate across
//! crates, hence this thin relay.

fn main() {
    println!("cargo:rustc-link-arg=-Wl,--disable-new-dtags");
    for var in ["DEP_FLUX_TEXT_RPATHS", "DEP_FLUX_RPATHS"] {
        if let Ok(rpaths) = std::env::var(var) {
            for dir in rpaths.split(';').filter(|s| !s.is_empty()) {
                println!("cargo:rustc-link-arg=-Wl,-rpath,{dir}");
            }
        }
    }
}
