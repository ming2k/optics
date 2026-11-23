//! Build script for `optics-svg-sys`.

use optics_build::{SysLibConfig, probe_and_link, write_bindings};

fn main() {
    let probed = probe_and_link(&SysLibConfig {
        pkg_name: "optics-svg",
        env_prefix: "OPTICS_SVG",
        dep_env_prefixes: &["OPTICS_GFX"],
        include_libs: &["svg", "gfx"],
        extra_pkg_config_includes: &[],
        headers: &["svg.h"],
    });

    let bindings = probed
        .bindgen_builder()
        .allowlist_function("optics_svg_.*")
        .allowlist_type("optics_svg_.*")
        .allowlist_var("OPTICS_SVG_.*")
        .allowlist_type("optics_gfx_color")
        .allowlist_type("optics_gfx_point")
        .allowlist_type("optics_gfx_rect")
        .allowlist_type("optics_gfx_gradient_stop")
        .allowlist_type("optics_gfx_gradient_stops")
        .allowlist_type("optics_gfx_line_cap")
        .allowlist_type("optics_gfx_line_join")
        .allowlist_type("optics_gfx_fill_rule")
        .allowlist_type("optics_gfx_result")
        .blocklist_type("optics_gfx_canvas")
        .blocklist_type("optics_gfx_encoder")
        .blocklist_type("optics_gfx_device")
        .blocklist_type("optics_gfx_image")
        .blocklist_type("optics_gfx_path")
        .blocklist_type("optics_gfx_arena")
        .blocklist_function("optics_gfx_.*")
        .blocklist_var("OPTICS_GFX_.*")
        .raw_line(
            "pub use optics_gfx_sys::{optics_gfx_arena, optics_gfx_canvas, optics_gfx_device, optics_gfx_encoder, \
             optics_gfx_image, optics_gfx_path};",
        )
        .raw_line("pub use optics_gfx_sys::optics_gfx_color_rgba_premul;")
        .newtype_enum("optics_svg_path_verb")
        .newtype_enum("optics_svg_pixel_format")
        .layout_tests(true)
        .generate()
        .expect("bindgen failed to generate svg bindings");

    write_bindings(&bindings);
}
