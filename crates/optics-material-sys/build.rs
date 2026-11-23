//! Build script for `optics-material-sys`.

use optics_build::{SysLibConfig, probe_and_link, write_bindings};

fn main() {
    let probed = probe_and_link(&SysLibConfig {
        pkg_name: "optics-material",
        env_prefix: "OPTICS_MATERIAL",
        dep_env_prefixes: &["OPTICS_GFX"],
        include_libs: &["material", "gfx"],
        extra_pkg_config_includes: &[],
        headers: &["material/liquid_glass.h", "material.h"],
    });

    let bindings = probed
        .bindgen_builder()
        .allowlist_function("optics_material_.*")
        .allowlist_type("optics_material_.*")
        .allowlist_var("OPTICS_MATERIAL_.*")
        .blocklist_type("optics_gfx_.*")
        .raw_line(
            "pub use optics_gfx_sys::{optics_gfx_device, optics_gfx_format, optics_gfx_frame, optics_gfx_image, \
             optics_gfx_point, optics_gfx_rect, optics_gfx_result};",
        )
        .layout_tests(true)
        .generate()
        .expect("bindgen failed to generate material bindings");

    write_bindings(&bindings);
}
