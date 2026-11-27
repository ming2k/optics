//! Build script for `optics-ui-sys`.

use optics_build::{SysLibConfig, probe_and_link, write_bindings};

fn main() {
    let probed = probe_and_link(&SysLibConfig {
        pkg_name: "optics-ui",
        env_prefix: "OPTICS_UI",
        dep_env_prefixes: &["OPTICS_TEXT", "OPTICS_GFX"],
        include_libs: &["ui", "text", "gfx"],
        extra_pkg_config_includes: &[],
        headers: &["ui.h", "ui/icon.h"],
    });

    let bindings = probed
        .bindgen_builder()
        .allowlist_function("optics_ui_.*")
        .allowlist_type("optics_ui")
        .allowlist_type("optics_ui_.*")
        .allowlist_var("OPTICS_UI_.*")
        // Opaque handle types and optics_gfx_result are blocklisted and
        // re-exported from optics-gfx-sys; POD value types embedded by-value in
        // ui structs are bound locally so bindgen can emit Copy/Clone/Debug
        // derives for those outer structs.
        .allowlist_type("optics_gfx_color")
        .allowlist_type("optics_gfx_point")
        .allowlist_type("optics_gfx_corner_radii")
        .allowlist_type("optics_gfx_rect")
        .blocklist_type("optics_gfx_canvas")
        .blocklist_type("optics_gfx_device")
        .blocklist_type("optics_gfx_image")
        .blocklist_type("optics_gfx_result")
        .blocklist_function("optics_gfx_.*")
        .blocklist_var("OPTICS_GFX_.*")
        .raw_line(
            "pub use optics_gfx_sys::{optics_gfx_canvas, optics_gfx_color_rgba_premul, \
             optics_gfx_device, optics_gfx_image, optics_gfx_result};",
        )
        // optics_ui_icon_id is extensible at runtime (optics_ui_icon_register hands
        // out generational handles beyond OPTICS_UI_ICON_BUILTIN_COUNT), so it must
        // be a transparent newtype.
        .newtype_enum("optics_ui_icon_id")
        .layout_tests(true)
        .generate()
        .expect("bindgen failed to generate ui bindings");

    write_bindings(&bindings);
}
