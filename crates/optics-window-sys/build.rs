//! Build script for `optics-window-sys`.

use optics_build::{SysLibConfig, probe_and_link, write_bindings};

fn main() {
    let probed = probe_and_link(&SysLibConfig {
        pkg_name: "optics-window",
        env_prefix: "OPTICS_WINDOW",
        dep_env_prefixes: &["OPTICS_GFX"],
        include_libs: &["window", "gfx"],
        extra_pkg_config_includes: &[],
        headers: &[
            "window.h",
            "window/loop.h",
            "window/state.h",
            "window/capability.h",
            "window/cursor.h",
            "window/dnd.h",
            "window/event.h",
            "window/host.h",
            "window/a11y_prefs.h",
            "window/theme.h",
            "window/file_dialog.h",
            "window/a11y.h",
        ],
    });

    let bindings = probed
        .bindgen_builder()
        // Only bindgen optics_window_* types and functions. optics_gfx_* types are bound by
        // optics-gfx-sys and re-exported below so one Rust definition exists.
        .allowlist_function("optics_window_.*")
        .allowlist_type("optics_window_.*")
        .allowlist_var("OPTICS_WINDOW_.*")
        .blocklist_type("optics_gfx_.*")
        .blocklist_function("optics_gfx_.*")
        .blocklist_var("OPTICS_GFX_.*")
        .raw_line(
            "pub use optics_gfx_sys::{optics_gfx_canvas, optics_gfx_color, optics_gfx_device, optics_gfx_encoder, \
             optics_gfx_frame, optics_gfx_point, optics_gfx_rect, optics_gfx_result};",
        )
        .layout_tests(false)
        .generate()
        .expect("bindgen failed to generate iris bindings");

    write_bindings(&bindings);
}
