//! Relay rpaths for `optics-scene` and link system image codecs.

fn main() {
    optics_build::relay_rpaths(
        &["DEP_OPTICS_SCENE_RPATHS", "DEP_OPTICS_GFX_RS_RPATHS"],
        true,
    );
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("windows") {
        println!("cargo:rustc-link-lib=png16");
        println!("cargo:rustc-link-lib=turbojpeg");
    }
}
