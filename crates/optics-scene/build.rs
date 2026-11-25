//! Relay rpaths for `optics-scene` and optionally link system image codecs.

fn main() {
    optics_build::relay_rpaths(
        &["DEP_OPTICS_SCENE_RPATHS", "DEP_OPTICS_GFX_RS_RPATHS"],
        true,
    );

    println!("cargo:rerun-if-env-changed=CARGO_FEATURE_SYSTEM_IMAGE_CODECS");
    if std::env::var_os("CARGO_FEATURE_SYSTEM_IMAGE_CODECS").is_some()
        && std::env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("windows")
    {
        for pkg in ["libpng", "libturbojpeg"] {
            optics_build::pkg_config::Config::new()
                .probe(pkg)
                .unwrap_or_else(|err| {
                    panic!(
                        "optics-scene `system-image-codecs` feature requires system `{pkg}` \
                         discoverable via pkg-config ({err}). Install the development package or \
                         disable `system-image-codecs` and pass a caller-supplied decoder to \
                         `Scene::from_glb_with_materials_and_decoder`."
                    )
                });
        }
    }
}
