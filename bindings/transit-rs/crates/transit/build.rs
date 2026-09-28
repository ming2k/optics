//! Re-emit the rpaths published by `transit-sys` (via its `links` metadata).

fn main() {
    let target_os = std::env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    if target_os == "windows" {
        return;
    }
    if target_os != "macos" {
        println!("cargo:rustc-link-arg=-Wl,--disable-new-dtags");
    }
    if let Ok(rpaths) = std::env::var("DEP_TRANSIT_RPATHS") {
        for dir in rpaths.split(';').filter(|s| !s.is_empty()) {
            println!("cargo:rustc-link-arg=-Wl,-rpath,{dir}");
        }
    }
}
