//! Build-time discovery and bindgen generation for libflux-scene-graph.

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
            dir.join("meson-uninstalled/flux-scene-graph-uninstalled.pc")
                .exists()
        });
    let source_dir = env::var_os("FLUX_SOURCE_DIR")
        .map(PathBuf::from)
        .or_else(|| checkout_root.as_ref().map(|root| root.join("libs/flux")))
        .map(normalize_flux_source_dir);

    let dev_mode = build_dir.as_ref().is_some_and(|dir| {
        !use_installed
            && dir
                .join("meson-uninstalled/flux-scene-graph-uninstalled.pc")
                .exists()
    });

    if dev_mode {
        let dir = build_dir.as_ref().unwrap();
        // libflux_scene_graph is built under libs/flux/scene_graph/; the
        // file name follows the target platform (libflux_scene_graph.so /
        // libflux_scene_graph.dylib / flux_scene_graph.dll).
        let library = dir
            .join("libs/flux/scene_graph")
            .join(shared_lib_file_name("flux_scene_graph", &os));
        if !library.exists() {
            panic!(
                "{} is missing; build optics with -Dscene-graph=true first",
                library.display()
            );
        }
        set_pkg_config_path(&[dir.join("meson-uninstalled")], &os);
    }

    // Enforce the MINIMUM C library version this crate's bindings assume
    // (the bindings share flux_sys handle types (0.0.30 seam)); pkg-config fails the build loudly when an older flux/lens/
    // iris/prism is picked up (e.g. a stale system install shadowing the
    // meson uninstalled dir).
    let lib = pkg_config::Config::new()
        .print_system_libs(false)
        .atleast_version("0.0.30")
        .probe("flux-scene-graph")
        .unwrap_or_else(|e| {
            panic!(
                "pkg-config failed for flux-scene-graph: {e}\n\
                 install the sibling library, set FLUX_BUILD_DIR to an optics build with \
                 -Dscene-graph=true, or set FLUX_USE_INSTALLED=1"
            )
        });

    let rpaths: Vec<String> = if dev_mode {
        lib.link_paths
            .iter()
            .map(|path| path.display().to_string())
            .collect()
    } else {
        Vec::new()
    };
    if os == "windows" {
        // Windows has no rpath: stage the DLLs (flux_scene_graph + its
        // flux dependency) next to the cargo profile output instead.
        // Runs in installed mode too — an installed prefix's bin/ is not
        // on PATH by default.
        stage_windows_dlls(&lib.link_paths, &["flux_scene_graph", "flux"]);
    } else {
        emit_rpath_link_args(&rpaths);
    }
    println!("cargo:rpaths={}", rpaths.join(";"));

    let mut clang_args: Vec<String> = lib
        .include_paths
        .iter()
        .map(|path| format!("-I{}", path.display()))
        .collect();
    if let Some(src) = &source_dir {
        clang_args.insert(
            0,
            format!("-I{}", src.join("scene_graph/include").display()),
        );
        clang_args.insert(0, format!("-I{}", src.join("include").display()));
    }

    let bindings = bindgen::Builder::default()
        .rust_edition(bindgen::RustEdition::Edition2024)
        .header("wrapper.h")
        .clang_args(&clang_args)
        .clang_arg("-std=c23")
        .allowlist_function("flux_sg_.*")
        .allowlist_type("flux_sg_.*")
        .allowlist_var("FLUX_SG_.*")
        // All flux types in this header cross as pointers or scalars. The
        // opaque handles (flux_device, flux_frame, flux_camera, ...) are
        // blocklisted and re-exported from flux_sys so exactly one Rust
        // definition of each exists across the stack; flux_result (a scalar
        // enum return) and flux_vec3-flavored PODs stay local — bindgen
        // cannot derive on blocklisted references and scalar enums carry no
        // seam risk.
        // flux_result is re-exported from flux_sys (see raw_line) so every
        // crate in the stack shares ONE error-code enum.
        .allowlist_type("flux_vec3")
        .blocklist_type("flux_result")
        .blocklist_type("flux_device")
        .blocklist_type("flux_frame")
        .blocklist_type("flux_camera")
        .blocklist_type("flux_material")
        .blocklist_type("flux_scene_light")
        .blocklist_type("flux_mesh")
        .blocklist_function("flux_(device|frame|camera|material|scene|mesh)_.*")
        .raw_line(
            "pub use flux_sys::{flux_camera, flux_device, flux_frame, flux_material, flux_result, flux_scene_light};",
        )
        .default_enum_style(bindgen::EnumVariation::Rust {
            non_exhaustive: false,
        })
        .derive_default(true)
        .derive_debug(true)
        .layout_tests(true)
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
        .generate()
        .expect("bindgen failed to generate flux-scene-graph bindings");

    let out = PathBuf::from(env::var("OUT_DIR").unwrap()).join("bindings.rs");
    bindings.write_to_file(out).expect("write bindings.rs");

    println!("cargo:rerun-if-changed=wrapper.h");
    if let Some(src) = &source_dir {
        println!(
            "cargo:rerun-if-changed={}",
            src.join("scene_graph/include/flux-scene-graph/scene-graph.h")
                .display()
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
