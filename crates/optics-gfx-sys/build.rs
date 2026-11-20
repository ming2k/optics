//! Build script for `optics-gfx-sys`.

use optics_build::{SysLibConfig, probe_and_link, write_bindings};

fn main() {
    let probed = probe_and_link(&SysLibConfig {
        pkg_name: "optics-gfx",
        env_prefix: "OPTICS_GFX",
        dep_env_prefixes: &[],
        include_libs: &["gfx"],
        extra_pkg_config_includes: &["vulkan"],
        headers: &[
            "gfx/core.h",
            "gfx/math.h",
            "gfx/canvas.h",
            "gfx/canvas_cpu.h",
            "gfx/vulkan.h",
            "gfx/dmabuf.h",
            "gfx/scene.h",
            "gfx/compute.h",
            "gfx/effect.h",
        ],
    });

    let bindings = probed
        .bindgen_builder()
        // Public flux surface only; Vk* types are pulled transitively as needed.
        .allowlist_function("optics_gfx_.*")
        .allowlist_type("optics_gfx_.*")
        .allowlist_var("OPTICS_GFX_.*")
        .layout_tests(true)
        .generate()
        .expect("bindgen failed to generate flux bindings");

    write_bindings(&bindings);
}
