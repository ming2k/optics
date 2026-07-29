//! Build script for `lens-sys`.
//!
//! Strategy mirrors flux-rs (out-of-tree, openssl-sys convention):
//!   1. Locate liblens via pkg-config. Two modes:
//!        - Installed (default): the standard pkg-config database finds
//!          `lens.pc` after `meson install` into a prefix on
//!          `PKG_CONFIG_PATH`.
//!        - Dev: set `LENS_BUILD_DIR=<lens-source>/build` so the meson
//!          build tree's `meson-uninstalled/*.pc` is prepended to
//!          `PKG_CONFIG_PATH`. If `FLUX_BUILD_DIR` is also set, its
//!          meson-uninstalled dir is prepended too so lens's
//!          `Requires: flux` resolves to the matching freshly-built
//!          libflux rather than any stale system copy. Optionally also
//!          set `LENS_SOURCE_DIR=<lens-source>` so bindgen reads headers
//!          straight from the source checkout.
//!   2. rpath each link dir (dev mode only) so test/example binaries
//!      find liblens.so / libflux.so without LD_LIBRARY_PATH.
//!   3. Run bindgen over `wrapper.h` using the probed include paths,
//!      prefixed by `LENS_SOURCE_DIR/libs/lens/include` when set.
//!
//! Override points (env):
//!   LENS_SOURCE_DIR    absolute path to the lens C source checkout; its
//!                      `libs/lens/include/` is preferred for bindgen.
//!   LENS_BUILD_DIR     absolute path to a meson build tree of that
//!                      checkout; its `meson-uninstalled/` subdir must
//!                      contain lens-uninstalled.pc.
//!   LENS_USE_INSTALLED set to `1` to skip the build tree and link the
//!                      system-installed lens.
//!   FLUX_BUILD_DIR     optional, dev mode for the flux dependency.
//!   PKG_CONFIG_PATH    respected and prepended-to, never clobbered.

use std::env;
use std::path::PathBuf;

fn main() {
    println!("cargo:rerun-if-env-changed=LENS_USE_INSTALLED");
    println!("cargo:rerun-if-env-changed=LENS_BUILD_DIR");
    println!("cargo:rerun-if-env-changed=LENS_SOURCE_DIR");
    println!("cargo:rerun-if-env-changed=FLUX_BUILD_DIR");
    println!("cargo:rerun-if-env-changed=PKG_CONFIG_PATH");

    let use_installed = env::var_os("LENS_USE_INSTALLED").is_some();
    let checkout_root = discover_optics_checkout("lens");
    let build_dir = env::var_os("LENS_BUILD_DIR")
        .map(PathBuf::from)
        .or_else(|| checkout_root.as_ref().map(|root| root.join("build")))
        .filter(|dir| dir.join("meson-uninstalled/lens-uninstalled.pc").exists());
    let source_dir = env::var_os("LENS_SOURCE_DIR")
        .map(PathBuf::from)
        .or_else(|| checkout_root.clone());

    // 1. Decide how to find lens.
    let dev_mode = if let Some(dir) = &build_dir {
        if use_installed {
            false
        } else {
            let pc = dir.join("meson-uninstalled/lens-uninstalled.pc");
            if !pc.exists() {
                panic!(
                    "LENS_BUILD_DIR is set to {}\n\
                     but its `meson-uninstalled/lens-uninstalled.pc` is missing.\n\
                     Reconfigure with:\n    \
                     meson setup {} -Dtests=false && meson compile -C {}\n\
                     or unset LENS_BUILD_DIR to link the system-installed lens,\n\
                     or set LENS_USE_INSTALLED=1.",
                    dir.display(),
                    dir.display(),
                    dir.display(),
                );
            }
            true
        }
    } else {
        false
    };

    if dev_mode {
        let dir = build_dir.as_ref().unwrap();
        // lens's own uninstalled tree first, then flux's (if pointed at),
        // so lens's `Requires: flux >= 0.1.0` resolves to the matching
        // freshly-built libflux rather than any stale system copy.
        let mut search = dir.join("meson-uninstalled").display().to_string();
        let flux_build_dir = env::var_os("FLUX_BUILD_DIR")
            .map(PathBuf::from)
            .or_else(|| checkout_root.as_ref().map(|root| root.join("build")));
        if let Some(flux_build_dir) = flux_build_dir {
            append_uninstalled_dir(&mut search, &flux_build_dir);
        }
        if let Some(existing) = env::var_os("PKG_CONFIG_PATH") {
            search.push(':');
            search.push_str(&existing.to_string_lossy());
        }
        // SAFETY: this build script is single-threaded, and this runs before
        // invoking pkg-config, bindgen, or any other code that may spawn threads.
        unsafe { env::set_var("PKG_CONFIG_PATH", &search) };
    }

    // 2. Probe lens (and transitively flux) via pkg-config. Emits link
    //    directives for cargo automatically.
    let lens = pkg_config::Config::new()
        .print_system_libs(false)
        .probe("lens")
        .unwrap_or_else(|e| {
            panic!(
                "pkg-config failed for lens: {e}\n\
                 Either:\n  \
                 - install lens (`meson install` into a prefix on \
                 PKG_CONFIG_PATH),\n  \
                 - set LENS_BUILD_DIR=<lens-source>/build (and optionally \
                 FLUX_BUILD_DIR) to link meson build trees, or\n  \
                 - set LENS_USE_INSTALLED=1.",
            )
        });

    // 3. rpath each link dir so binaries run from the build tree directly
    //    (dev mode only — installed libraries resolve via the loader / ldconfig).
    //    `cargo:rustc-link-arg` applies to this crate's own targets only;
    //    publish the dirs as `links` metadata so dependents re-emit them
    //    (DEP_LENS_RPATHS).
    if dev_mode {
        let rpaths: Vec<String> = lens
            .link_paths
            .iter()
            .map(|p| p.display().to_string())
            .collect();
        for dir in &rpaths {
            println!("cargo:rustc-link-arg=-Wl,-rpath,{dir}");
        }
        println!("cargo:rpaths={}", rpaths.join(";"));
    }

    // 4. Bindgen over the header. Include paths come from pkg-config; if
    //    LENS_SOURCE_DIR is set, prepend its `libs/lens/include` so the
    //    generated bindings match that exact source checkout.
    let mut clang_args: Vec<String> = lens
        .include_paths
        .iter()
        .map(|p| format!("-I{}", p.display()))
        .collect();
    if let Some(src) = &source_dir {
        clang_args.push(format!("-I{}", src.join("libs/lens/include").display()));
    }

    let bindings = bindgen::Builder::default()
        .rust_edition(bindgen::RustEdition::Edition2024)
        .header("wrapper.h")
        .clang_args(&clang_args)
        .clang_arg("-std=c23")
        .allowlist_function("lens_.*")
        .allowlist_type("lens_.*")
        .allowlist_var("LENS_.*")
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
        .expect("bindgen failed to generate lens bindings");

    let out = PathBuf::from(env::var("OUT_DIR").unwrap()).join("bindings.rs");
    bindings.write_to_file(&out).expect("write bindings.rs");

    println!("cargo:rerun-if-changed=wrapper.h");
    if let Some(src) = &source_dir {
        for h in ["lens.h", "icon.h"] {
            println!(
                "cargo:rerun-if-changed={}",
                src.join("libs/lens/include/lens").join(h).display()
            );
        }
    }
}

fn discover_optics_checkout(component: &str) -> Option<PathBuf> {
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").ok()?);
    for ancestor in manifest_dir.ancestors() {
        if ancestor
            .join(format!("libs/{component}/include/{component}"))
            .exists()
            && ancestor.join("build/meson-uninstalled").exists()
        {
            return Some(ancestor.to_path_buf());
        }
    }
    None
}

fn append_uninstalled_dir(search: &mut String, build_dir: &std::path::Path) {
    let uninstalled = build_dir.join("meson-uninstalled");
    if uninstalled.exists() {
        search.push(':');
        search.push_str(&uninstalled.display().to_string());
    }
}
