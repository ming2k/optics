//! Build script for `iris-sys`.
//!
//! Strategy mirrors flux-rs / lens-rs (out-of-tree, openssl-sys convention):
//!   1. Locate libiris via pkg-config. Two modes:
//!        - Installed (default): the standard pkg-config database finds
//!          `iris.pc` after `meson install` into a prefix on
//!          `PKG_CONFIG_PATH`.
//!        - Dev: set `IRIS_BUILD_DIR=<iris-source>/build` so the meson
//!          build tree's `meson-uninstalled/*.pc` is prepended to
//!          `PKG_CONFIG_PATH`. iris depends on lens (which depends on
//!          flux), so when dev mode is on we also prepend LENS_BUILD_DIR
//!          and FLUX_BUILD_DIR's uninstalled dirs — otherwise pkg-config
//!          resolves `Requires: lens >= 0.1.0` against any stale system
//!          copy. Optionally also set `IRIS_SOURCE_DIR=<iris-source>` so
//!          bindgen reads headers straight from the source checkout.
//!   2. Publish non-system link dirs as rpath metadata (`cargo:rpaths`)
//!      so the `iris` crate's build.rs can relay them to its own targets.
//!   3. Run bindgen over `wrapper.h` using the probed include paths,
//!      prefixed by `IRIS_SOURCE_DIR/libs/iris/include` when set.
//!
//! Override points (env):
//!   IRIS_SOURCE_DIR    absolute path to the iris C source checkout; its
//!                      `libs/iris/include/` is preferred for bindgen.
//!   IRIS_BUILD_DIR     absolute path to a meson build tree of that
//!                      checkout; its `meson-uninstalled/` subdir must
//!                      contain iris-uninstalled.pc.
//!   IRIS_USE_INSTALLED set to `1` to skip the build tree and link the
//!                      system-installed iris.
//!   LENS_BUILD_DIR     optional, dev mode for the lens dependency.
//!   FLUX_BUILD_DIR     optional, dev mode for the flux dependency.
//!   PKG_CONFIG_PATH    respected and prepended-to, never clobbered.

use std::env;
use std::path::PathBuf;

fn main() {
    println!("cargo:rerun-if-env-changed=IRIS_USE_INSTALLED");
    println!("cargo:rerun-if-env-changed=IRIS_BUILD_DIR");
    println!("cargo:rerun-if-env-changed=IRIS_SOURCE_DIR");
    println!("cargo:rerun-if-env-changed=LENS_BUILD_DIR");
    println!("cargo:rerun-if-env-changed=FLUX_BUILD_DIR");
    println!("cargo:rerun-if-env-changed=PKG_CONFIG_PATH");

    let use_installed = env::var_os("IRIS_USE_INSTALLED").is_some();
    let checkout_root = discover_optics_checkout("iris");
    let build_dir = env::var_os("IRIS_BUILD_DIR")
        .map(PathBuf::from)
        .or_else(|| checkout_root.as_ref().map(|root| root.join("build")))
        .filter(|dir| dir.join("meson-uninstalled/iris-uninstalled.pc").exists());
    let source_dir = env::var_os("IRIS_SOURCE_DIR")
        .map(PathBuf::from)
        .or_else(|| checkout_root.clone());

    let dev_mode = if let Some(dir) = &build_dir {
        if use_installed {
            false
        } else {
            let pc = dir.join("meson-uninstalled/iris-uninstalled.pc");
            if !pc.exists() {
                panic!(
                    "IRIS_BUILD_DIR is set to {}\n\
                     but its `meson-uninstalled/iris-uninstalled.pc` is missing.\n\
                     Reconfigure with:\n    \
                     meson setup {} -Dtests=false && meson compile -C {}\n\
                     or unset IRIS_BUILD_DIR to link the system-installed iris,\n\
                     or set IRIS_USE_INSTALLED=1.",
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
        // iris's own uninstalled tree first, then lens's and flux's (iris
        // Requires lens, lens Requires flux — both must be reachable so
        // pkg-config does not fall back to stale system copies).
        let mut search = dir.join("meson-uninstalled").display().to_string();
        for var in ["LENS_BUILD_DIR", "FLUX_BUILD_DIR"] {
            let other = env::var_os(var)
                .map(PathBuf::from)
                .or_else(|| checkout_root.as_ref().map(|root| root.join("build")));
            if let Some(other) = other {
                append_uninstalled_dir(&mut search, &other);
            }
        }
        if let Some(existing) = env::var_os("PKG_CONFIG_PATH") {
            search.push(':');
            search.push_str(&existing.to_string_lossy());
        }
        // SAFETY: this build script is single-threaded, and this runs before
        // invoking pkg-config, bindgen, or any other code that may spawn threads.
        unsafe { env::set_var("PKG_CONFIG_PATH", &search) };
    }

    let iris = pkg_config::Config::new()
        .print_system_libs(false)
        .probe("iris")
        .unwrap_or_else(|e| {
            panic!(
                "pkg-config failed for iris: {e}\n\
                 Either:\n  \
                 - install iris (`meson install` into a prefix on \
                 PKG_CONFIG_PATH),\n  \
                 - set IRIS_BUILD_DIR=<iris-source>/build (and optionally \
                 LENS_BUILD_DIR / FLUX_BUILD_DIR) to link meson build trees, or\n  \
                 - set IRIS_USE_INSTALLED=1.",
            )
        });

    // Publish non-system link dirs as rpath metadata so the `iris` crate's
    // build.rs can relay them to its own targets (examples / unit tests /
    // benches). `cargo:rustc-link-arg` emitted here would NOT reach those —
    // rustc-link-arg from a -sys crate applies only to that crate's own
    // targets (iris-sys has none) — so we publish via the `links = "iris"`
    // metadata channel and the iris crate re-emits DEP_IRIS_RPATHS. This
    // mirrors flux-sys's `cargo:rpaths` mechanism. System installs (/usr/lib,
    // /lib) need no rpath; custom prefixes (~/.flux-prefix, $PREFIX) do, so
    // `cargo run --example` / `cargo test` find libiris + liblens + libflux
    // without LD_LIBRARY_PATH.
    let rpaths: Vec<String> = iris
        .link_paths
        .iter()
        .filter(|p| !is_system_libdir(p))
        .map(|p| p.display().to_string())
        .collect();
    println!("cargo:rpaths={}", rpaths.join(";"));

    let mut clang_args: Vec<String> = iris
        .include_paths
        .iter()
        .map(|p| format!("-I{}", p.display()))
        .collect();
    if let Some(src) = &source_dir {
        clang_args.push(format!("-I{}", src.join("libs/iris/include").display()));
    }

    let bindings = bindgen::Builder::default()
        .rust_edition(bindgen::RustEdition::Edition2024)
        .header("wrapper.h")
        .clang_args(&clang_args)
        .clang_arg("-std=c23")
        // Only bindgen iris_* types and functions. lens_* (lens) and flux_*
        // types are referenced in the C signatures but bound from their own
        // -sys crates at the Rust seam — we just see opaque void pointers
        // in their place.
        .allowlist_function("iris_.*")
        .allowlist_type("iris_.*")
        .allowlist_var("IRIS_.*")
        .default_enum_style(bindgen::EnumVariation::Rust {
            non_exhaustive: false,
        })
        .derive_default(true)
        .derive_debug(true)
        .layout_tests(false)
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
        .generate()
        .expect("bindgen failed to generate iris bindings");

    let out = PathBuf::from(env::var("OUT_DIR").unwrap()).join("bindings.rs");
    bindings.write_to_file(&out).expect("write bindings.rs");

    println!("cargo:rerun-if-changed=wrapper.h");
    if let Some(src) = &source_dir {
        for h in [
            "iris.h",
            "app.h",
            "cursor.h",
            "theme.h",
            "file_dialog.h",
            "a11y.h",
        ] {
            println!(
                "cargo:rerun-if-changed={}",
                src.join("libs/iris/include/iris").join(h).display()
            );
        }
    }
}

/// True for dynamic-loader default search dirs (/lib, /lib64, /lib/<triplet>,
/// /usr/lib, /usr/lib64, /usr/lib/<triplet>). Link dirs under these need no
/// rpath; everything else (custom prefixes) does.
fn is_system_libdir(p: &std::path::Path) -> bool {
    let Ok(s) = p.canonicalize() else {
        return false;
    };
    let s = match s.to_str() {
        Some(s) => s,
        None => return false,
    };
    s == "/lib"
        || s == "/lib64"
        || s == "/usr/lib"
        || s == "/usr/lib64"
        || s.strip_prefix("/lib/").is_some()
        || s.strip_prefix("/usr/lib/").is_some()
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
