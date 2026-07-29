//! Build script for `flux-text-sys`.
//!
//! Mirrors `crates/flux-sys/build.rs` but for the flux-text sibling C
//! library: probe pkg-config `flux-text` (which Requires flux, so
//! libflux's headers and link line come along), point bindgen at the
//! header so the bindings match the requested source checkout, and
//! publish the build-tree rpath so dependent crates' test/example
//! binaries find libflux_text.so (and libflux.so) without LD_LIBRARY_PATH.
//!
//! Override points (env) — same shape as flux-sys:
//!   FLUX_SOURCE_DIR    absolute path to either the optics checkout root or
//!                      its `libs/flux` subtree; flux and flux-text headers
//!                      are preferred for bindgen.
//!   FLUX_BUILD_DIR     absolute path to a meson build tree of that
//!                      checkout; its `meson-uninstalled/` subdir must
//!                      contain flux-text-uninstalled.pc.
//!   FLUX_USE_INSTALLED set to `1` to skip the build tree and link the
//!                      system-installed flux-text.
//!   PKG_CONFIG_PATH    respected and prepended-to, never clobbered.

use std::env;
use std::path::PathBuf;

fn main() {
    println!("cargo:rerun-if-env-changed=FLUX_USE_INSTALLED");
    println!("cargo:rerun-if-env-changed=FLUX_BUILD_DIR");
    println!("cargo:rerun-if-env-changed=FLUX_SOURCE_DIR");
    println!("cargo:rerun-if-env-changed=PKG_CONFIG_PATH");

    let use_installed = env::var_os("FLUX_USE_INSTALLED").is_some();
    let checkout_root = discover_optics_checkout();
    let build_dir = env::var_os("FLUX_BUILD_DIR")
        .map(PathBuf::from)
        .or_else(|| checkout_root.as_ref().map(|root| root.join("build")))
        .filter(|dir| {
            dir.join("meson-uninstalled/flux-text-uninstalled.pc")
                .exists()
        });
    let source_dir = env::var_os("FLUX_SOURCE_DIR")
        .map(PathBuf::from)
        .or_else(|| checkout_root.as_ref().map(|root| root.join("libs/flux")))
        .or_else(|| {
            Some(PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap()).join("../../../../"))
        })
        .map(normalize_flux_source_dir);

    let dev_mode = if let Some(dir) = &build_dir {
        if use_installed {
            false
        } else {
            let uninstalled = dir.join("meson-uninstalled");
            let pc = uninstalled.join("flux-text-uninstalled.pc");
            let exists = pc.exists();
            if exists {
                // libflux_text.so is built under libs/flux/text/ (meson subdir),
                // but earlier versions built it in the root or libs/flux-text/.
                let probe_old = dir.join("libs/flux-text/libflux_text.so");
                let probe_new = dir.join("libs/flux/text/libflux_text.so");
                if !probe_old.exists() && !probe_new.exists() {
                    panic!(
                        "stale or incomplete meson build dir at {}\n\
                         Its `flux-text-uninstalled.pc` exists but \
                         `libflux_text.so` is absent — build the C sibling \
                         first:\n    \
                         meson setup {} -Dtext=true && meson compile -C {}\n\
                         or set FLUX_BUILD_DIR, or FLUX_USE_INSTALLED=1.",
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
        // SAFETY: this build script is single-threaded, and this runs before
        // invoking pkg-config, bindgen, or any other code that may spawn threads.
        unsafe { env::set_var("PKG_CONFIG_PATH", &search) };
    }

    let lib = pkg_config::Config::new()
        .print_system_libs(false)
        .probe("flux-text")
        .unwrap_or_else(|e| {
            panic!(
                "pkg-config failed for flux-text: {e}\n\
                 Either:\n  \
                 - install flux-text (`meson install` into a prefix on \
                 PKG_CONFIG_PATH),\n  \
                 - set FLUX_BUILD_DIR=<flux-source>/build (with -Dtext=true), \
                 or\n  \
                 - set FLUX_USE_INSTALLED=1.",
            )
        });

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
    // Publish to dependents as DEP_FLUX_TEXT_RPATHS (links = "flux-text").
    println!("cargo:rpaths={}", rpaths.join(";"));

    let mut clang_args: Vec<String> = lib
        .include_paths
        .iter()
        .map(|p| format!("-I{}", p.display()))
        .collect();
    // Source-checkout headers first when requested — reliable regardless of
    // any stale -I baked into the uninstalled .pc.
    if let Some(src) = &source_dir {
        clang_args.insert(0, format!("-I{}", src.join("text/include").display()));
        clang_args.insert(0, format!("-I{}", src.join("include").display()));
    }

    let bindings = bindgen::Builder::default()
        .rust_edition(bindgen::RustEdition::Edition2024)
        .header("wrapper.h")
        .clang_args(&clang_args)
        .clang_arg("-std=c23")
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
        .expect("bindgen failed to generate flux-text bindings");

    let out = PathBuf::from(env::var("OUT_DIR").unwrap()).join("bindings.rs");
    bindings.write_to_file(&out).expect("write bindings.rs");

    println!("cargo:rerun-if-changed=wrapper.h");
    if let Some(src) = &source_dir {
        println!(
            "cargo:rerun-if-changed={}",
            src.join("text/include/flux-text/text.h").display()
        );
    }
}

fn normalize_flux_source_dir(dir: PathBuf) -> PathBuf {
    if dir.join("libs/flux/include/flux").exists() {
        dir.join("libs/flux")
    } else {
        dir
    }
}

/// Walk up from this crate's manifest dir looking for the optics checkout:
/// identified by `libs/flux/include/flux` plus a meson `build/` tree. Mirrors
/// `flux-sys/build.rs` so out-of-tree consumers (e.g. typio) auto-discover a
/// dev build tree without setting FLUX_BUILD_DIR.
fn discover_optics_checkout() -> Option<PathBuf> {
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").ok()?);
    for ancestor in manifest_dir.ancestors() {
        if ancestor.join("libs/flux/include/flux").exists()
            && ancestor.join("build/meson-uninstalled").exists()
        {
            return Some(ancestor.to_path_buf());
        }
    }
    None
}
