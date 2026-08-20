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
//!   2. Make the runtime loader find the freshly-built shared library
//!      (dev mode only): `-Wl,-rpath` on unix (incl. macOS, where meson
//!      uses `@rpath/` install names), DLL staging next to the cargo
//!      profile output on Windows — no `LD_LIBRARY_PATH` / PATH needed.
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
    // OS we are linking for (build scripts run on the host; the linkage —
    // library file names, list separators, rpath vs DLL staging — must
    // match the target).
    let os = target_os();
    let checkout_root = discover_optics_checkout();
    let build_dir = env::var_os("FLUX_BUILD_DIR")
        .map(PathBuf::from)
        .or_else(|| checkout_root.as_ref().map(|root| root.join("build")))
        .filter(|dir| dir.join("meson-uninstalled/flux-uninstalled.pc").exists());
    let source_dir = env::var_os("FLUX_SOURCE_DIR")
        .map(PathBuf::from)
        .or_else(|| checkout_root.as_ref().map(|root| root.join("libs/flux")));

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
                // Sanity-check the build tree actually contains the flux
                // shared library (libflux.so / libflux.dylib / flux.dll).
                // A stale dir (repo moved/copied after `meson setup`) still
                // has the .pc but no library; surface it early instead of
                // letting pkg-config / bindgen emit confusing errors.
                let lib_name = shared_lib_file_name("flux", &os);
                let probe_root = dir.join(&lib_name);
                let probe_libs = dir.join("libs/flux").join(&lib_name);
                if !probe_root.exists() && !probe_libs.exists() {
                    panic!(
                        "stale or incomplete meson build dir at {}\n\
                         Its `flux-uninstalled.pc` exists but `{lib_name}` is \
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
        set_pkg_config_path(&[dir.join("meson-uninstalled")], &os);
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

    // 3. Runtime library discovery (dev mode only — installed libraries
    //    resolve via the loader / ldconfig). `rustc-link-arg` applies to
    //    this crate's own targets only; publish the dirs as `links`
    //    metadata so dependents re-emit them (DEP_FLUX_RPATHS).
    let rpaths: Vec<String> = if dev_mode {
        lib.link_paths
            .iter()
            .map(|p| p.display().to_string())
            .collect()
    } else {
        Vec::new()
    };
    if os == "windows" {
        // Windows has no rpath: stage the DLLs next to the cargo profile
        // output so test/example binaries find them. Runs in installed
        // mode too — an installed prefix's bin/ is not on PATH by default.
        stage_windows_dlls(&lib.link_paths, &["flux"]);
    } else {
        emit_rpath_link_args(&rpaths);
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
        .rust_edition(bindgen::RustEdition::Edition2024)
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
            "canvas_cpu.h",
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
