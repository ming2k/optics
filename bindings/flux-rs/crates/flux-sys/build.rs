//! Build script for `flux-sys`.
//!
//! Strategy (out-of-tree, follows the openssl-sys / rusqlite convention):
//!   1. Locate libflux via pkg-config. Two modes:
//!        - Installed (default): the standard pkg-config database finds
//!          `flux.pc` after `meson install` into a prefix on
//!          `PKG_CONFIG_PATH`.
//!        - Dev: set `FLUX_BUILD_DIR=<flux-source>/build` so the meson
//!          build tree's `meson-uninstalled/*.pc` is prepended to
//!          `PKG_CONFIG_PATH`. Optionally also set
//!          `FLUX_SOURCE_DIR=<flux-source>` so bindgen reads headers
//!          straight from the source checkout (overrides any stale -I
//!          baked into the uninstalled .pc).
//!   2. Bake an rpath for each link path (dev mode only) so test/example
//!      binaries find the .so at runtime without `LD_LIBRARY_PATH`.
//!   3. Run bindgen over `wrapper.h` using the probed include paths,
//!      prefixed by `FLUX_SOURCE_DIR/include` when set so the generated
//!      bindings always match the requested source checkout.
//!
//! Override points (env):
//!   FLUX_SOURCE_DIR    absolute path to the flux C source checkout; its
//!                      `include/` directory is preferred for bindgen.
//!   FLUX_BUILD_DIR     absolute path to a meson build tree of that
//!                      checkout; its `meson-uninstalled/` subdir must
//!                      contain flux-uninstalled.pc.
//!   FLUX_USE_INSTALLED set to `1` to skip the build tree and link the
//!                      system-installed flux. Also chosen automatically
//!                      when FLUX_BUILD_DIR is unset or its .pc is absent.
//!   PKG_CONFIG_PATH    respected and prepended-to, never clobbered.

use std::env;
use std::path::PathBuf;

fn main() {
    println!("cargo:rerun-if-env-changed=FLUX_USE_INSTALLED");
    println!("cargo:rerun-if-env-changed=FLUX_BUILD_DIR");
    println!("cargo:rerun-if-env-changed=FLUX_SOURCE_DIR");
    println!("cargo:rerun-if-env-changed=PKG_CONFIG_PATH");

    let use_installed = env::var_os("FLUX_USE_INSTALLED").is_some();
    let build_dir = env::var_os("FLUX_BUILD_DIR").map(PathBuf::from);
    let source_dir = env::var_os("FLUX_SOURCE_DIR").map(PathBuf::from);

    // 1. Decide how to find flux.
    //    - Dev: FLUX_BUILD_DIR points at a meson build tree whose
    //      meson-uninstalled .pc is prepended to PKG_CONFIG_PATH so we
    //      link the freshly-built library without `meson install`.
    //    - Installed: FLUX_USE_INSTALLED=1, or FLUX_BUILD_DIR is unset,
    //      or its .pc is absent. We then rely on the system pkg-config
    //      database (flux.pc from a `meson install` prefix).
    let dev_mode = if let Some(dir) = &build_dir {
        if use_installed {
            false
        } else {
            let uninstalled = dir.join("meson-uninstalled");
            let pc = uninstalled.join("flux-uninstalled.pc");
            let exists = pc.exists();
            if exists {
                // Sanity-check the build tree actually contains libflux.so.
                // A stale dir (repo moved/copied after `meson setup`) still
                // has the .pc but no library; surface it early instead of
                // letting pkg-config / bindgen emit confusing errors.
                let probe = dir.join("libflux.so");
                if !probe.exists() {
                    panic!(
                        "stale or incomplete meson build dir at {}\n\
                         Its `flux-uninstalled.pc` exists but `libflux.so` is \
                         absent — either `meson compile` has not run yet, or the \
                         build tree no longer matches the source checkout.\n\
                         Reconfigure with:\n    \
                         meson setup {} -Dtests=false && meson compile -C {}\n\
                         or set FLUX_BUILD_DIR to a fresh build directory,\n\
                         or set FLUX_USE_INSTALLED=1 to link the system-installed \
                         flux.",
                        dir.display(),
                        dir.display(),
                        dir.display(),
                    );
                }
            }
            exists
        }
    } else {
        false
    };

    if dev_mode {
        let dir = build_dir.as_ref().unwrap();
        let uninstalled = dir.join("meson-uninstalled");
        let mut search = uninstalled.display().to_string();
        if let Some(existing) = env::var_os("PKG_CONFIG_PATH") {
            search.push(':');
            search.push_str(&existing.to_string_lossy());
        }
        env::set_var("PKG_CONFIG_PATH", &search);
    } else if !use_installed {
        // Fall through to pkg-config; if `flux` is absent there, the probe
        // below will emit the actionable error.
    }

    // 2. Probe. Emits rustc-link-lib / rustc-link-search for flux automatically.
    let lib = pkg_config::Config::new()
        .print_system_libs(false)
        .probe("flux")
        .unwrap_or_else(|e| {
            panic!(
                "pkg-config failed for flux: {e}\n\
                 Either:\n  \
                 - install flux (`meson install` into a prefix on \
                 PKG_CONFIG_PATH),\n  \
                 - set FLUX_BUILD_DIR=<flux-source>/build to link a meson \
                 build tree, or\n  \
                 - set FLUX_USE_INSTALLED=1 to skip the build-tree probe.",
            )
        });

    // 3. rpath each link dir so binaries run from the build tree directly
    //    (dev mode only — installed libraries resolve via the loader / ldconfig).
    //    `rustc-link-arg` applies to this crate's own targets only; publish the
    //    dirs as `links` metadata so dependents re-emit them (DEP_FLUX_RPATHS).
    let rpaths: Vec<String> = if dev_mode {
        lib.link_paths
            .iter()
            .map(|p| p.display().to_string())
            .collect()
    } else {
        Vec::new()
    };
    for dir in &rpaths {
        println!("cargo:rustc-link-arg=-Wl,-rpath,{dir}");
    }
    println!("cargo:rpaths={}", rpaths.join(";"));

    // 4. Generate bindings. Include paths come from pkg-config; if
    //    FLUX_SOURCE_DIR is set, prepend its `include/` so the generated
    //    bindings match that exact source checkout regardless of any stale
    //    -I baked into the uninstalled .pc.
    let mut clang_args: Vec<String> = lib
        .include_paths
        .iter()
        .map(|p| format!("-I{}", p.display()))
        .collect();
    if let Some(src) = &source_dir {
        clang_args.insert(0, format!("-I{}", src.join("include").display()));
    }
    // flux lists `Requires.private: vulkan`; ensure the Vulkan headers are
    // reachable even if the private cflags did not surface them.
    if let Ok(vk) = pkg_config::Config::new()
        .print_system_libs(false)
        .probe("vulkan")
    {
        for p in &vk.include_paths {
            clang_args.push(format!("-I{}", p.display()));
        }
    }

    let bindings = bindgen::Builder::default()
        .header("wrapper.h")
        .clang_args(&clang_args)
        .clang_arg("-std=c23")
        // Public flux surface only; Vk* types are pulled transitively as needed.
        .allowlist_function("flux_.*")
        .allowlist_type("flux_.*")
        .allowlist_var("FLUX_.*")
        .default_enum_style(bindgen::EnumVariation::Rust {
            non_exhaustive: false,
        })
        .derive_default(true)
        .derive_debug(true)
        .layout_tests(true)
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
        .generate()
        .expect("bindgen failed to generate bindings");

    let out = PathBuf::from(env::var("OUT_DIR").unwrap()).join("bindings.rs");
    bindings.write_to_file(&out).expect("write bindings.rs");

    // Re-run when inputs change.
    println!("cargo:rerun-if-changed=wrapper.h");
    if let Some(src) = &source_dir {
        for h in [
            "core.h",
            "math.h",
            "canvas.h",
            "vulkan.h",
            "dmabuf.h",
            "scene.h",
            "compute.h",
            "effect.h",
        ] {
            println!(
                "cargo:rerun-if-changed={}",
                src.join("include/flux").join(h).display()
            );
        }
    }
}
