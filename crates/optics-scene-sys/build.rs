//! Build script for `optics-scene-sys`.

use optics_build::{SysLibConfig, probe_and_link, write_bindings};

fn main() {
    let probed = probe_and_link(&SysLibConfig {
        pkg_name: "optics-scene",
        env_prefix: "OPTICS_SCENE",
        dep_env_prefixes: &["OPTICS_GFX"],
        include_libs: &["scene", "gfx"],
        extra_pkg_config_includes: &[],
        headers: &["scene.h"],
    });

    let bindings = probed
        .bindgen_builder()
        .allowlist_function("optics_scene_.*")
        .allowlist_type("optics_scene")
        .allowlist_type("optics_scene_.*")
        .allowlist_var("OPTICS_SCENE_.*")
        .blocklist_type("optics_gfx_.*")
        .blocklist_function("optics_gfx_.*")
        .blocklist_var("OPTICS_GFX_.*")
        .raw_line(
            "pub use optics_gfx_sys::{optics_gfx_camera, optics_gfx_device, optics_gfx_frame, \
             optics_gfx_material, optics_gfx_mesh, optics_gfx_result, optics_gfx_scene_light, \
             optics_gfx_vec3};",
        )
        .layout_tests(true)
        .generate()
        .expect("bindgen failed to generate scene bindings");

    write_bindings(&bindings);
}
