//! Build script for `aperture-sys`.
//!
//! Same strategy as `iris-sys` / `lens-sys` (out-of-tree, openssl-sys
//! convention): locate libaperture via pkg-config (installed, or a meson build
//! tree's `meson-uninstalled/` in dev mode), publish non-system link dirs as
//! rpath metadata for the safe crate to relay, and run bindgen over
//! `wrapper.h`.
//!
//! Override points (env):
//!   APERTURE_SOURCE_DIR    optics source checkout; its headers are preferred.
//!   APERTURE_BUILD_DIR     meson build tree containing
//!                          `meson-uninstalled/aperture-uninstalled.pc`.
//!   APERTURE_USE_INSTALLED set to `1` to link the system-installed library.
//!   OPTICS_BUILD_DIR       fallback build tree for every optics -sys crate.
//!   PKG_CONFIG_PATH        respected and prepended-to, never clobbered.

use std::env;
use std::path::PathBuf;

fn main() {
    println!("cargo:rerun-if-env-changed=APERTURE_USE_INSTALLED");
    println!("cargo:rerun-if-env-changed=APERTURE_BUILD_DIR");
    println!("cargo:rerun-if-env-changed=APERTURE_SOURCE_DIR");
    println!("cargo:rerun-if-env-changed=LENS_BUILD_DIR");
    println!("cargo:rerun-if-env-changed=FLUX_BUILD_DIR");
    println!("cargo:rerun-if-env-changed=OPTICS_SOURCE_DIR");
    println!("cargo:rerun-if-env-changed=OPTICS_BUILD_DIR");
    println!("cargo:rerun-if-env-changed=PKG_CONFIG_PATH");

    let use_installed = env::var_os("APERTURE_USE_INSTALLED").is_some();
    // OS we are linking for (build scripts run on the host; the linkage —
    // list separators, rpath vs DLL staging — must match the target).
    let os = target_os();
    let checkout_root = discover_optics_checkout("aperture");
    let build_dir = env::var_os("APERTURE_BUILD_DIR")
        .or_else(|| env::var_os("OPTICS_BUILD_DIR"))
        .map(PathBuf::from)
        .or_else(|| checkout_root.as_ref().map(|root| root.join("build")))
        .filter(|dir| dir.join("meson-uninstalled/aperture-uninstalled.pc").exists());
    let source_dir = env::var_os("APERTURE_SOURCE_DIR")
        .map(PathBuf::from)
        .or_else(|| checkout_root.clone());

    let dev_mode = if let Some(dir) = &build_dir {
        if use_installed {
            false
        } else {
            let pc = dir.join("meson-uninstalled/aperture-uninstalled.pc");
            if !pc.exists() {
                panic!(
                    "APERTURE_BUILD_DIR is set to {}\n\
                     but its `meson-uninstalled/aperture-uninstalled.pc` is missing.\n\
                     Reconfigure with:\n    \
                     meson setup {} -Dtests=false && meson compile -C {}\n\
                     or unset APERTURE_BUILD_DIR to link the system-installed aperture,\n\
                     or set APERTURE_USE_INSTALLED=1.",
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
        let mut dirs = vec![dir.join("meson-uninstalled")];
        for var in ["LENS_BUILD_DIR", "FLUX_BUILD_DIR"] {
            let other = env::var_os(var)
                .map(PathBuf::from)
                .or_else(|| checkout_root.as_ref().map(|root| root.join("build")));
            if let Some(other) = other {
                let uninstalled = other.join("meson-uninstalled");
                if uninstalled.exists() && !dirs.contains(&uninstalled) {
                    dirs.push(uninstalled);
                }
            }
        }
        set_pkg_config_path(&dirs, &os);
    }

    // Pre-1.0 has no stable ABI: the bindings are generated from this
    // crate's exact headers, so the C library must be the same X.Y.Z.
    // A stale system install fails the build here instead of loading at
    // runtime with a mismatched descriptor layout.
    let aperture = pkg_config::Config::new()
        .print_system_libs(false)
        .exactly_version(env!("CARGO_PKG_VERSION"))
        .probe("aperture")
        .unwrap_or_else(|e| {
            panic!(
                "pkg-config failed for aperture: {e}\n\
                 Either:\n  \
                 - install optics (`meson install` into a prefix on \
                 PKG_CONFIG_PATH),\n  \
                 - set APERTURE_BUILD_DIR=<optics-source>/build (and optionally \
                 LENS_BUILD_DIR / FLUX_BUILD_DIR) to link meson build trees, or\n  \
                 - set APERTURE_USE_INSTALLED=1.",
            )
        });

    // Publish non-system link dirs as rpath metadata so the `aperture` crate's
    // build.rs can relay them to its own targets (examples / unit tests /
    // benches). `cargo:rustc-link-arg` emitted here would NOT reach those —
    // rustc-link-arg from a -sys crate applies only to that crate's own
    // targets (aperture-sys has none) — so we publish via the `links = "aperture"`
    // metadata channel and the aperture crate re-emits DEP_APERTURE_RPATHS. This
    // mirrors flux-sys's `cargo:rpaths` mechanism. System installs (/usr/lib,
    // /lib) need no rpath; custom prefixes (~/.flux-prefix, $PREFIX) do, so
    // `cargo run --example` / `cargo test` find libaperture + liblens + libflux
    // without LD_LIBRARY_PATH.
    let rpaths: Vec<String> = aperture
        .link_paths
        .iter()
        .filter(|p| !is_system_libdir(p, &os))
        .map(|p| p.display().to_string())
        .collect();
    println!("cargo:rpaths={}", rpaths.join(";"));
    if os == "windows" {
        // Windows has no rpath: stage the DLLs (iris + its lens/flux
        // dependencies) next to the cargo profile output so test/example
        // binaries find them. Runs in installed mode too — an installed
        // prefix's bin/ is not on PATH by default.
        stage_windows_dlls(&aperture.link_paths, &["aperture", "iris", "lens", "flux"]);
    }

    let mut clang_args: Vec<String> = aperture
        .include_paths
        .iter()
        .map(|p| format!("-I{}", p.display()))
        .collect();
    if let Some(src) = &source_dir {
        clang_args.insert(0, format!("-I{}", src.join("libs/aperture/include").display()));
        clang_args.insert(1, format!("-I{}", src.join("libs/iris/include").display()));
        clang_args.insert(2, format!("-I{}", src.join("libs/lens/include").display()));
        clang_args.insert(3, format!("-I{}", src.join("libs/flux/include").display()));
    }

    let bindings = bindgen::Builder::default()
        .rust_edition(bindgen::RustEdition::Edition2024)
        .header("wrapper.h")
        .clang_args(&clang_args)
        .clang_arg("-std=c23")
        // Only bindgen aperture_* types and functions. Everything else the
        // signatures mention is bound by the crate that owns it and
        // re-exported below, so exactly one Rust definition of each type
        // exists across the stack.
        .allowlist_function("aperture_.*")
        .allowlist_type("aperture_.*")
        .blocklist_type("flux_.*")
        .blocklist_function("flux_.*")
        .blocklist_var("FLUX_.*")
        .blocklist_type("iris_.*")
        .blocklist_function("iris_.*")
        .blocklist_var("IRIS_.*")
        .blocklist_type("lens")
        .blocklist_type("lens_.*")
        .blocklist_function("lens_.*")
        .blocklist_var("LENS_.*")
        .raw_line("pub use flux_sys::{flux_device, flux_encoder};")
        .raw_line("pub use iris_sys::{iris_event_fn, iris_prepare_fn, iris_record_fn};")
        .raw_line("pub use lens_sys::{lens, lens_input};")
        .default_enum_style(bindgen::EnumVariation::Rust {
            non_exhaustive: false,
        })
        .derive_default(true)
        .derive_debug(true)
        .layout_tests(false)
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
        .generate()
        .expect("bindgen failed to generate aperture bindings");

    let out = PathBuf::from(env::var("OUT_DIR").unwrap()).join("bindings.rs");
    bindings.write_to_file(&out).expect("write bindings.rs");

    println!("cargo:rerun-if-changed=wrapper.h");
    if let Some(src) = &source_dir {
        println!(
            "cargo:rerun-if-changed={}",
            src.join("libs/aperture/include/aperture/aperture.h").display()
        );
    }
}

/// True for dynamic-loader default search dirs. On unix that is /lib,
/// /lib64, /lib/<triplet>, /usr/lib, /usr/lib64, /usr/lib/<triplet>
/// (macOS is likewise covered: dyld falls back to /usr/lib and
/// /usr/local/lib — the latter is *not* matched here, so Homebrew
/// prefixes still get an rpath, which is what we want). Link dirs under
/// these need no rpath; everything else (custom prefixes) does.
/// Windows has no applicable fixed system libdir (System32 is never a
/// pkg-config link dir), so nothing is filtered out there.
fn is_system_libdir(p: &std::path::Path, target_os: &str) -> bool {
    if target_os == "windows" {
        return false;
    }
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
