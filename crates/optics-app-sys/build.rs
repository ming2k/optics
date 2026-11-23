//! Build script for `optics-app-sys`.

use optics_build::{SysLibConfig, probe_and_link, write_bindings};

fn main() {
    let probed = probe_and_link(&SysLibConfig {
        pkg_name: "optics-app",
        env_prefix: "OPTICS_APP",
        dep_env_prefixes: &[
            "OPTICS_WINDOW",
            "OPTICS_UI",
            "OPTICS_TEXT",
            "OPTICS_SVG",
            "OPTICS_GFX",
        ],
        include_libs: &["app", "window", "ui", "text", "svg", "gfx"],
        extra_pkg_config_includes: &[],
        headers: &["app.h"],
    });

    let bindings = probed
        .bindgen_builder()
        .allowlist_function("optics_app_.*")
        .allowlist_type("optics_app_.*")
        .blocklist_type("optics_gfx_.*")
        .blocklist_function("optics_gfx_.*")
        .blocklist_var("OPTICS_GFX_.*")
        .blocklist_type("optics_window_.*")
        .blocklist_function("optics_window_.*")
        .blocklist_var("OPTICS_WINDOW_.*")
        .blocklist_type("optics_ui")
        .blocklist_type("optics_ui_.*")
        .blocklist_function("optics_ui_.*")
        .blocklist_var("OPTICS_UI_.*")
        .raw_line("pub use optics_gfx_sys::{optics_gfx_device, optics_gfx_encoder};")
        .raw_line("pub use optics_window_sys::{optics_window_event_fn, optics_window_prepare_fn, optics_window_record_fn};")
        .raw_line("pub use optics_ui_sys::{optics_ui, optics_ui_input};")
        .layout_tests(false)
        .generate()
        .expect("bindgen failed to generate app bindings");

    write_bindings(&bindings);
}
