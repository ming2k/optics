//! Build script for `flux-text-sys`.
//!
//! Mirrors `crates/flux-sys/build.rs` but for the flux-text sibling C
//! library: probe pkg-config `flux-text` (which Requires flux, so
//! libflux's headers and link line come along), point bindgen at the
//! header so the bindings match the requested source checkout, and
//! publish the runtime search dirs (rpath on unix, staged DLLs on
//! Windows) so dependent crates' test/example binaries find the
//! flux-text/flux shared libraries without LD_LIBRARY_PATH / PATH.
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
    // OS we are linking for (build scripts run on the host; the linkage —
    // library file names, list separators, rpath vs DLL staging — must
    // match the target).
    let os = target_os();
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
                // libflux_text is built under libs/flux/text/ (meson subdir),
                // but earlier versions built it in the root or libs/flux-text/.
                // The file name follows the target platform:
                // libflux_text.so / libflux_text.dylib / flux_text.dll.
                let lib_name = shared_lib_file_name("flux_text", &os);
                let probe_old = dir.join("libs/flux-text").join(&lib_name);
                let probe_new = dir.join("libs/flux/text").join(&lib_name);
                if !probe_old.exists() && !probe_new.exists() {
                    panic!(
                        "stale or incomplete meson build dir at {}\n\
                         Its `flux-text-uninstalled.pc` exists but \
                         `{lib_name}` is absent — build the C sibling \
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
        set_pkg_config_path(&[dir.join("meson-uninstalled")], &os);
    }

    // Enforce the MINIMUM C library version this crate's bindings assume
    // (the bindings share flux_sys handle types (0.0.29 seam)); pkg-config fails the build loudly when an older flux/lens/
    // iris/prism is picked up (e.g. a stale system install shadowing the
    // meson uninstalled dir).
    let lib = pkg_config::Config::new()
        .print_system_libs(false)
        .atleast_version("0.0.29")
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
    if os == "windows" {
        // Windows has no rpath: stage the DLLs (flux_text + its flux
        // dependency) next to the cargo profile output instead. Runs in
        // installed mode too — an installed prefix's bin/ is not on PATH
        // by default.
        stage_windows_dlls(&lib.link_paths, &["flux_text", "flux"]);
    } else {
        emit_rpath_link_args(&rpaths);
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
        .allowlist_function("flux_text_.*")
        .allowlist_type("flux_text_.*")
        .allowlist_var("FLUX_TEXT_.*")
        // Flux seam types text's API borrows. Opaque handles (flux_device,
        // flux_canvas, flux_arena, flux_text itself stays local as it IS
        // this crate's subject) are re-exported from flux_sys so exactly one
        // Rust definition of each exists across the stack. The by-value POD
        // flux_color is bound locally: bindgen cannot derive Copy/Clone on
        // structs referencing blocklisted types, and duplicating a repr(C)
        // u32 alias carries no seam risk.
        .allowlist_type("flux_color")
        .blocklist_type("flux_device")
        .blocklist_type("flux_canvas")
        .blocklist_type("flux_arena")
        .blocklist_type("flux_result")
        .blocklist_function("flux_(canvas|device|arena)_.*")
        .raw_line("pub use flux_sys::{flux_arena, flux_canvas, flux_device, flux_result};")
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
/// `flux-sys/build.rs` so out-of-tree consumers auto-discover a
/// dev build tree without setting FLUX_BUILD_DIR.
fn discover_optics_checkout() -> Option<PathBuf> {
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").ok()?);
    for ancestor in manifest_dir.ancestors() {
        if ancestor.join("libs/flux/include/flux").exists() {
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
