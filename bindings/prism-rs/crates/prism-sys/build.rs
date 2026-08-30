//! Build script for `prism-sys`.
//!
//! Strategy mirrors flux-sys / lens-sys (out-of-tree, openssl-sys
//! convention):
//!   1. Locate libprism via pkg-config. Two modes:
//!        - Installed (default): the standard pkg-config database finds
//!          `prism.pc` after `meson install` into a prefix on
//!          `PKG_CONFIG_PATH`.
//!        - Dev: set `PRISM_BUILD_DIR=<optics>/build` so the meson build
//!          tree's `meson-uninstalled/*.pc` is prepended to
//!          `PKG_CONFIG_PATH`. prism's `Requires: flux` then resolves to the
//!          matching freshly-built libflux from the same tree rather than
//!          any stale system copy. Optionally also set
//!          `PRISM_SOURCE_DIR=<optics>/libs/prism` so bindgen reads headers
//!          straight from the source checkout (overrides any stale -I baked
//!          into the uninstalled .pc).
//!   2. Make the runtime loader find the freshly-built shared libraries
//!      (dev mode only): `-Wl,-rpath` on unix (incl. macOS, where meson
//!      uses `@rpath/` install names), DLL staging next to the cargo
//!      profile output on Windows — no `LD_LIBRARY_PATH` / PATH needed.
//!   3. Run bindgen over `wrapper.h` using the probed include paths,
//!      prefixed by `PRISM_SOURCE_DIR/include` when set so the generated
//!      bindings always match the requested source checkout.
//!
//! flux type sharing: prism's API speaks flux types (`flux_device *`,
//! `flux_result`, `flux_rect`, ...). bindgen allowlists only `prism_*` items
//! and BLOCKLISTS `flux_*` types: references to a blocklisted type are still
//! emitted under their plain C name, and the `raw_line` below re-exports the
//! `flux_sys` originals, so `flux_result` and friends have exactly one Rust
//! definition across the stack (shared with the safe `flux` crate).
//!
//! Override points (env):
//!   PRISM_SOURCE_DIR    absolute path to the prism C source checkout (the
//!                       directory containing `include/`); its `include/`
//!                       directory is preferred for bindgen.
//!   PRISM_BUILD_DIR     absolute path to a meson build tree of that
//!                       checkout; its `meson-uninstalled/` subdir must
//!                       contain prism-uninstalled.pc.
//!   PRISM_USE_INSTALLED set to `1` to skip the build tree and link the
//!                       system-installed prism. Also chosen automatically
//!                       when PRISM_BUILD_DIR is unset or its .pc is absent.
//!   FLUX_BUILD_DIR      optional, dev mode for the flux dependency (defaults
//!                       to the same build tree as prism).
//!   PKG_CONFIG_PATH     respected and prepended-to, never clobbered.

use std::env;
use std::path::PathBuf;

fn main() {
    println!("cargo:rerun-if-env-changed=PRISM_USE_INSTALLED");
    println!("cargo:rerun-if-env-changed=PRISM_BUILD_DIR");
    println!("cargo:rerun-if-env-changed=PRISM_SOURCE_DIR");
    println!("cargo:rerun-if-env-changed=FLUX_BUILD_DIR");
    println!("cargo:rerun-if-env-changed=PKG_CONFIG_PATH");

    let use_installed = env::var_os("PRISM_USE_INSTALLED").is_some();
    // OS we are linking for (build scripts run on the host; the linkage —
    // library file names, list separators, rpath vs DLL staging — must
    // match the target).
    let os = target_os();
    let checkout_root = discover_optics_checkout();
    let build_dir = env::var_os("PRISM_BUILD_DIR")
        .map(PathBuf::from)
        .or_else(|| checkout_root.as_ref().map(|root| root.join("build")))
        .filter(|dir| dir.join("meson-uninstalled/prism-uninstalled.pc").exists());
    let source_dir = env::var_os("PRISM_SOURCE_DIR")
        .map(PathBuf::from)
        .or_else(|| checkout_root.as_ref().map(|root| root.join("libs/prism")));

    // 1. Decide how to find prism.
    //    - Dev: PRISM_BUILD_DIR points at a meson build tree whose
    //      meson-uninstalled .pc is prepended to PKG_CONFIG_PATH so we
    //      link the freshly-built library without `meson install`.
    //    - Installed: PRISM_USE_INSTALLED=1, or PRISM_BUILD_DIR is unset,
    //      or its .pc is absent. We then rely on the system pkg-config
    //      database (prism.pc from a `meson install` prefix).
    let dev_mode = if let Some(dir) = &build_dir {
        if use_installed {
            false
        } else {
            let uninstalled = dir.join("meson-uninstalled");
            let pc = uninstalled.join("prism-uninstalled.pc");
            let exists = pc.exists();
            if exists {
                // Sanity-check the build tree actually contains the prism
                // shared library (libprism.so / libprism.dylib / prism.dll).
                // A stale dir (repo moved/copied after `meson setup`) still
                // has the .pc but no library; surface it early instead of
                // letting pkg-config / bindgen emit confusing errors.
                let lib_name = shared_lib_file_name("prism", &os);
                let probe_root = dir.join(&lib_name);
                let probe_libs = dir.join("libs/prism").join(&lib_name);
                if !probe_root.exists() && !probe_libs.exists() {
                    panic!(
                        "stale or incomplete meson build dir at {}\n\
                         Its `prism-uninstalled.pc` exists but `{lib_name}` is \
                         absent — either `meson compile` has not run yet, or the \
                         build tree no longer matches the source checkout.\n\
                         Reconfigure with:\n    \
                         meson setup {} -Dtests=false && meson compile -C {}\n\
                         or set PRISM_BUILD_DIR to a fresh build directory,\n\
                         or set PRISM_USE_INSTALLED=1 to link the system-installed \
                         prism.",
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
        // prism's own uninstalled tree first, then flux's (if pointed at),
        // so prism's `Requires: flux >= 0.0.30` resolves to the matching
        // freshly-built libflux rather than any stale system copy. In the
        // optics monorepo both live in the same build tree, so the two dirs
        // usually coincide.
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
    } else if !use_installed {
        // Fall through to pkg-config; if `prism` is absent there, the probe
        // below will emit the actionable error.
    }

    // 2. Probe. prism's `Requires: flux` pulls libflux's link directives in
    //    automatically; emits rustc-link-lib / rustc-link-search for both.
    // Enforce the MINIMUM C library version this crate's bindings assume
    // (prism_version_string exists (0.0.30)); pkg-config fails the build loudly when an older flux/lens/
    // iris/prism is picked up (e.g. a stale system install shadowing the
    // meson uninstalled dir).
    let lib = pkg_config::Config::new()
        .print_system_libs(false)
        .atleast_version("0.0.30")
        .probe("prism")
        .unwrap_or_else(|e| {
            panic!(
                "pkg-config failed for prism: {e}\n\
                 Either:\n  \
                 - install prism (`meson install` into a prefix on \
                 PKG_CONFIG_PATH),\n  \
                 - set PRISM_BUILD_DIR=<optics>/build to link a meson \
                 build tree, or\n  \
                 - set PRISM_USE_INSTALLED=1 to skip the build-tree probe.",
            )
        });

    // 3. Runtime library discovery (dev mode only — installed libraries
    //    resolve via the loader / ldconfig). `rustc-link-arg` applies to
    //    this crate's own targets only; publish the dirs as `links`
    //    metadata so dependents re-emit them (DEP_PRISM_RPATHS). The probed
    //    link paths cover flux's build dir too, so one rpath set resolves
    //    both shared libraries.
    let rpaths: Vec<String> = if dev_mode {
        lib.link_paths
            .iter()
            .map(|p| p.display().to_string())
            .collect()
    } else {
        Vec::new()
    };
    if os == "windows" {
        // Windows has no rpath: stage the DLLs (prism + its flux dependency)
        // next to the cargo profile output so test/example binaries find
        // them. Runs in installed mode too — an installed prefix's bin/ is
        // not on PATH by default.
        stage_windows_dlls(&lib.link_paths, &["prism", "flux"]);
    } else {
        emit_rpath_link_args(&rpaths);
    }
    println!("cargo:rpaths={}", rpaths.join(";"));

    // 4. Generate bindings. Include paths come from pkg-config (prism's and,
    //    via `Requires: flux`, flux's); if PRISM_SOURCE_DIR is set, prepend
    //    its `include/` so the generated bindings match that exact source
    //    checkout regardless of any stale -I baked into the uninstalled .pc.
    let mut clang_args: Vec<String> = lib
        .include_paths
        .iter()
        .map(|p| format!("-I{}", p.display()))
        .collect();
    if let Some(src) = &source_dir {
        clang_args.insert(0, format!("-I{}", src.join("include").display()));
    }

    let bindings = bindgen::Builder::default()
        .rust_edition(bindgen::RustEdition::Edition2024)
        .header("wrapper.h")
        .clang_args(&clang_args)
        .clang_arg("-std=c23")
        // Public prism surface only. The flux types its signatures speak are
        // blocklisted: bindgen still emits references under their plain C
        // names, and the raw_line re-exports the flux-sys originals, so one
        // Rust definition of flux_result / flux_image / ... is shared across
        // the stack (and with the safe `flux` crate).
        .allowlist_function("prism_.*")
        .allowlist_type("prism_.*")
        .allowlist_var("PRISM_.*")
        .blocklist_type("flux_.*")
        .raw_line(
            "pub use flux_sys::{flux_device, flux_frame, flux_image, flux_point, flux_rect, flux_result};",
        )
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
        for h in ["liquid_glass.h", "prism.h"] {
            println!(
                "cargo:rerun-if-changed={}",
                src.join("include/prism").join(h).display()
            );
        }
    }
}

fn discover_optics_checkout() -> Option<PathBuf> {
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").ok()?);
    for ancestor in manifest_dir.ancestors() {
        if ancestor.join("libs/prism/include/prism").exists() {
            return Some(ancestor.to_path_buf());
        }
    }
    None
}

// ---------------------------------------------------------------------------
// Cross-platform plumbing, keyed off CARGO_CFG_TARGET_OS (the OS we link
// for — build scripts themselves run on the host):
//   windows          -> <stem>.dll,     ';' path lists, no rpath (stage DLLs)
//   macos            -> lib<stem>.dylib, ':' path lists, -Wl,-rpath (LC_RPATH)
//   linux/other unix -> lib<stem>.so,    ':' path lists, -Wl,-rpath (DT_RPATH)
// ---------------------------------------------------------------------------

/// OS we are linking for (`CARGO_CFG_TARGET_OS`); defaults to linux.
fn target_os() -> String {
    env::var("CARGO_CFG_TARGET_OS").unwrap_or_else(|_| "linux".to_string())
}

/// Meson's shared-library file name for `stem` on the target:
/// `lib<stem>.so` on Linux/other unix, `lib<stem>.dylib` on macOS,
/// `<stem>.dll` on Windows (meson drops the `lib` prefix there).
fn shared_lib_file_name(stem: &str, target_os: &str) -> String {
    match target_os {
        "windows" => format!("{stem}.dll"),
        "macos" => format!("lib{stem}.dylib"),
        _ => format!("lib{stem}.so"),
    }
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
