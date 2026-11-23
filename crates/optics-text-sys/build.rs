//! Build script for `optics-text-sys`.

use optics_build::{SysLibConfig, probe_and_link, write_bindings};

fn main() {
    let probed = probe_and_link(&SysLibConfig {
        pkg_name: "optics-text",
        env_prefix: "OPTICS_TEXT",
        dep_env_prefixes: &["OPTICS_GFX"],
        include_libs: &["text", "gfx"],
        extra_pkg_config_includes: &[],
        headers: &["text.h"],
    });

    let bindings = probed
        .bindgen_builder()
        .allowlist_function("optics_text_.*")
        .allowlist_type("optics_text_.*")
        .allowlist_var("OPTICS_TEXT_.*")
        .blocklist_type("optics_gfx_.*")
        .blocklist_function("optics_gfx_.*")
        .blocklist_var("OPTICS_GFX_.*")
        .raw_line(
            "pub use optics_gfx_sys::{optics_gfx_arena, optics_gfx_canvas, optics_gfx_color, \
             optics_gfx_device, optics_gfx_encoder, optics_gfx_frame, optics_gfx_result};",
        )
        .layout_tests(true)
        .generate()
        .expect("bindgen failed to generate text bindings");

    write_bindings(&bindings);
}
