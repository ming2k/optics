//! Re-emit the rpaths published by `lens-sys` (via its `links` metadata) so
//! that *this* crate's binaries — integration tests and examples — find
//! liblens.so (and libflux.so, which lives in flux's build dir) in the meson
//! build tree at runtime without `LD_LIBRARY_PATH`. `rustc-link-arg` does not
//! propagate across crates, hence this thin relay. Mirrors the `flux` crate's
//! build.rs.

fn main() {
    println!("cargo:rustc-link-arg=-Wl,--disable-new-dtags");
    for var in ["DEP_LENS_RPATHS", "DEP_FLUX_RPATHS"] {
        if let Ok(rpaths) = std::env::var(var) {
            for dir in rpaths.split(';').filter(|s| !s.is_empty()) {
                println!("cargo:rustc-link-arg=-Wl,-rpath,{dir}");
            }
        }
    }
}
