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
//!   2. Make the runtime loader find the freshly-built liblens/libflux
//!      (dev mode only): `-Wl,-rpath` on unix, staged DLLs next to the
//!      cargo profile output on Windows — no LD_LIBRARY_PATH / PATH.
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
    println!("cargo:rerun-if-env-changed=OPTICS_SOURCE_DIR");
    println!("cargo:rerun-if-env-changed=OPTICS_BUILD_DIR");
    println!("cargo:rerun-if-env-changed=PKG_CONFIG_PATH");

    let use_installed = env::var_os("LENS_USE_INSTALLED").is_some();
    // OS we are linking for (build scripts run on the host; the linkage —
    // list separators, rpath vs DLL staging — must match the target).
    let os = target_os();
    let checkout_root = discover_optics_checkout("lens");
    let build_dir = env::var_os("LENS_BUILD_DIR")
        .or_else(|| env::var_os("OPTICS_BUILD_DIR"))
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
        // so lens's `Requires: flux >= 0.0.29` resolves to the matching
        // freshly-built libflux rather than any stale system copy.
        let mut dirs = vec![dir.join("meson-uninstalled")];
        let flux_build_dir = env::var_os("FLUX_BUILD_DIR")
            .map(PathBuf::from)
            .or_else(|| checkout_root.as_ref().map(|root| root.join("build")));
        if let Some(flux_build_dir) = flux_build_dir {
            let uninstalled = flux_build_dir.join("meson-uninstalled");
            if uninstalled.exists() {
                dirs.push(uninstalled);
            }
        }
        set_pkg_config_path(&dirs, &os);
    }

    // 2. Probe lens (and transitively flux) via pkg-config. Emits link
    //    directives for cargo automatically.
    // Enforce the MINIMUM C library version this crate's bindings assume
    // (lens_version_string derives from macros; label family collapsed (0.0.29 surface)); pkg-config fails the build loudly when an older flux/lens/
    // iris/prism is picked up (e.g. a stale system install shadowing the
    // meson uninstalled dir).
    let lens = pkg_config::Config::new()
        .print_system_libs(false)
        .atleast_version("0.0.29")
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

    // 3. Runtime library discovery. `cargo:rustc-link-arg` applies to this
    //    crate's own targets only; publish the dirs as `links` metadata so
    //    dependents re-emit them (DEP_LENS_RPATHS). Unconditional (not just
    //    dev mode): a custom `meson install` prefix needs an rpath exactly
    //    like a build tree; system libdirs are filtered out.
    if os == "windows" {
        // Windows has no rpath: stage the DLLs (lens + its flux
        // dependency) next to the cargo profile output instead. Runs in
        // installed mode too — an installed prefix's bin/ is not on PATH
        // by default.
        stage_windows_dlls(&lens.link_paths, &["lens", "flux"]);
    } else {
        let rpaths: Vec<String> = lens
            .link_paths
            .iter()
            .filter(|p| !is_system_libdir(p, &os))
            .map(|p| p.display().to_string())
            .collect();
        emit_rpath_link_args(&rpaths);
        println!("cargo:rpaths={}", rpaths.join(";"));
    }
    let _ = dev_mode;

    // 4. Bindgen over the header. Include paths come from pkg-config; if
    //    LENS_SOURCE_DIR is set, prepend its `libs/lens/include` so the
    //    generated bindings match that exact source checkout.
    let mut clang_args: Vec<String> = lens
        .include_paths
        .iter()
        .map(|p| format!("-I{}", p.display()))
        .collect();
    if let Some(src) = &source_dir {
        clang_args.insert(0, format!("-I{}", src.join("libs/lens/include").display()));
        clang_args.insert(1, format!("-I{}", src.join("libs/flux/include").display()));
    }

    let bindings = bindgen::Builder::default()
        .rust_edition(bindgen::RustEdition::Edition2024)
        .header("wrapper.h")
        .clang_args(&clang_args)
        .clang_arg("-std=c23")
        .allowlist_function("lens_.*")
        .allowlist_type("lens_.*")
        .allowlist_var("LENS_.*")
        // flux seam types, split by how they cross the boundary:
        //  - Opaque HANDLE types (flux_canvas, flux_device, flux_image) are
        //    blocklisted and re-exported from flux_sys via the raw_line
        //    below, so exactly one Rust definition of each handle exists
        //    across the stack — pointers to them cross crate seams without
        //    casts.
        //  - VALUE types embedded by-value in lens structs (flux_color,
        //    flux_point, flux_rect) and the flux_result enum are bound
        //    locally: bindgen cannot emit derives for structs whose fields
        //    reference blocklisted types, and duplicating POD value types
        //    carries no seam risk (both sides are plain repr(C) data).
        .allowlist_type("flux_color")
        .allowlist_type("flux_point")
        .allowlist_type("flux_rect")
        .allowlist_type("flux_result")
        .blocklist_type("flux_canvas")
        .blocklist_type("flux_device")
        .blocklist_type("flux_image")
        .blocklist_function("flux_.*")
        .blocklist_var("FLUX_.*")
        .raw_line("pub use flux_sys::{flux_canvas, flux_device, flux_image};")
        .raw_line("pub use flux_sys::flux_color_rgba_premul;")
        .default_enum_style(bindgen::EnumVariation::Rust {
            non_exhaustive: false,
        })
        // lens_icon_id is extensible at runtime (lens_icon_register_svg
        // hands out ids beyond LENS_ICON_COUNT), so it must not be a
        // fieldless Rust enum — holding an unlisted discriminant would be
        // immediate UB. The transparent newtype admits every u32.
        .newtype_enum("lens_icon_id")
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
    // Explicit OPTICS_SOURCE_DIR wins: out-of-tree consumers (git deps,
    // registry builds) are not inside the monorepo, so the upward walk
    // below cannot find the checkout — one variable points at it.
    if let Some(root) = env::var_os("OPTICS_SOURCE_DIR") {
        let root = PathBuf::from(root);
        if root
            .join(format!("libs/{component}/include/{component}"))
            .exists()
        {
            return Some(root);
        }
    }
    for ancestor in manifest_dir.ancestors() {
        if ancestor
            .join(format!("libs/{component}/include/{component}"))
            .exists()
        {
            return Some(ancestor.to_path_buf());
        }
    }
    None
}

// ---------------------------------------------------------------------------
// Cross-platform plumbing, keyed off CARGO_CFG_TARGET_OS (the OS we link
// for — build scripts themselves run on the host):
//   windows          -> ';' path lists, no rpath (stage DLLs)
//   macos            -> ':' path lists, -Wl,-rpath (LC_RPATH)
//   linux/other unix -> ':' path lists, -Wl,-rpath (DT_RPATH)
// ---------------------------------------------------------------------------

/// OS we are linking for (`CARGO_CFG_TARGET_OS`); defaults to linux.
fn target_os() -> String {
    env::var("CARGO_CFG_TARGET_OS").unwrap_or_else(|_| "linux".to_string())
}

/// Overwrite PKG_CONFIG_PATH with `dirs` followed by any pre-existing value,
/// joined with the target's list separator: ';' on Windows (pkgconf
/// convention), ':' on unix (freedesktop pkg-config).
fn set_pkg_config_path(dirs: &[PathBuf], target_os: &str) {
    let sep = if target_os == "windows" { ';' } else { ':' };
    let mut search = dirs
        .iter()
        .map(|d| d.display().to_string())
        .collect::<Vec<_>>()
        .join(&sep.to_string());
    if let Some(existing) = env::var_os("PKG_CONFIG_PATH") {
        search.push(sep);
        search.push_str(&existing.to_string_lossy());
    }
    // SAFETY: this build script is single-threaded, and this runs before
    // invoking pkg-config, bindgen, or any other code that may spawn threads.
    unsafe { env::set_var("PKG_CONFIG_PATH", search) };
}

/// Emit `-Wl,-rpath,<dir>` for each dir. On macOS the same spelling is
/// accepted by ld64 and recorded as LC_RPATH; it resolves meson-built
/// dylibs because their install name is `@rpath/<libname>`.
fn emit_rpath_link_args(rpaths: &[String]) {
    for dir in rpaths {
        println!("cargo:rustc-link-arg=-Wl,-rpath,{dir}");
    }
}

/// Windows runtime discovery: no rpath exists, so the loader must find the
/// DLLs in the executable's directory or on PATH. Copy the DLLs we link
/// against into the cargo profile directory (target/<profile>/, derived from
/// OUT_DIR = target/<profile>/build/<pkg>-<hash>/out) plus its `deps/` and
/// `examples/` subdirs, where test and example binaries land. Copy failures
/// downgrade to `cargo:warning` — adding the DLL dir to PATH still works.
fn stage_windows_dlls(link_dirs: &[PathBuf], stems: &[&str]) {
    let Ok(out_dir) = env::var("OUT_DIR") else {
        return;
    };
    let Some(profile_dir) = PathBuf::from(&out_dir)
        .ancestors()
        .nth(3)
        .map(PathBuf::from)
    else {
        println!(
            "cargo:warning=cannot derive the cargo profile dir from OUT_DIR={out_dir}; \
             DLLs not staged — add the meson library dir to PATH"
        );
        return;
    };
    let mut destinations = vec![profile_dir.clone()];
    for sub in ["deps", "examples"] {
        let dir = profile_dir.join(sub);
        if dir.is_dir() || std::fs::create_dir_all(&dir).is_ok() {
            destinations.push(dir);
        }
    }
    for link_dir in link_dirs {
        // An installed prefix keeps DLLs in bin/ next to the link dir lib/;
        // a meson build tree has them right in the link dir.
        let search_dirs = [link_dir.clone(), link_dir.join("../bin")];
        for stem in stems {
            // Meson names the DLL <stem>.dll on Windows; accept the
            // MinGW-style lib<stem>.dll as a fallback.
            let mut dll = search_dirs.iter().flat_map(|dir| {
                [
                    dir.join(format!("{stem}.dll")),
                    dir.join(format!("lib{stem}.dll")),
                ]
            });
            let Some(dll) = dll.find(|p| p.is_file()) else {
                continue;
            };
            for dest in &destinations {
                if let Err(e) = std::fs::copy(&dll, dest.join(dll.file_name().unwrap())) {
                    println!(
                        "cargo:warning=failed to copy {} to {}: {e}; \
                         add {} to PATH so the loader finds it",
                        dll.display(),
                        dest.display(),
                        link_dir.display()
                    );
                }
            }
        }
    }
}

/// True for loader-default library directories (/usr/lib, /lib64, …) that
/// never need an rpath. Homebrew's /usr/local/lib is deliberately NOT
/// filtered: it is not on the default loader path on Linux.
fn is_system_libdir(path: &std::path::Path, os: &str) -> bool {
    if os == "windows" {
        return false;
    }
    let text = path.to_string_lossy();
    let parts: Vec<&str> = text.split('/').filter(|s| !s.is_empty()).collect();
    if parts.len() >= 2
        && (parts[0] == "usr" || parts[0] == "lib")
        && (parts[1] == "lib" || parts[1] == "lib64" || parts[1] == "lib32")
    {
        return true;
    }
    if parts.len() == 1 && parts[0].starts_with("lib") {
        return true; // /lib, /lib64, /lib32
    }
    false
}
