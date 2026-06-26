//! Re-emit the rpaths published by `flux-text-sys` (via its `links` metadata)
//! so that *this* crate's binaries — unit tests — find libflux_text.so (and
//! libflux.so, which lives in the same build dir) without LD_LIBRARY_PATH.
//! `rustc-link-arg` does not propagate across crates, hence this relay.

fn main() {
    if let Ok(rpaths) = std::env::var("DEP_FLUX_TEXT_RPATHS") {
        println!("cargo:rustc-link-arg=-Wl,--disable-new-dtags");
        for dir in rpaths.split(';').filter(|s| !s.is_empty()) {
            println!("cargo:rustc-link-arg=-Wl,-rpath,{dir}");
        }
    }
}
