//! Re-emit the rpaths published by enabled upstream wrapper crates (`flux`,
//! `iris`, `aperture`) so `optics` test and example binaries find the newly
//! built shared libraries in the meson tree without `LD_LIBRARY_PATH` or a
//! prior system install.

use std::collections::BTreeSet;

fn main() {
    let target_os = std::env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    if target_os == "windows" {
        return;
    }

    let mut dirs = BTreeSet::new();
    for var in [
        "DEP_APERTURE_RS_RPATHS",
        "DEP_IRIS_RS_RPATHS",
        "DEP_FLUX_RS_RPATHS",
    ] {
        if let Ok(val) = std::env::var(var) {
            for dir in val.split(';').filter(|s| !s.is_empty()) {
                dirs.insert(dir.to_owned());
            }
        }
    }

    if dirs.is_empty() {
        return;
    }
    if target_os != "macos" {
        println!("cargo:rustc-link-arg=-Wl,--disable-new-dtags");
    }
    for dir in dirs {
        println!("cargo:rustc-link-arg=-Wl,-rpath,{dir}");
    }
}
