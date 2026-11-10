//! Build script for `svg-sys`.
//!
//! Same out-of-tree strategy as `lens-sys` / `transit-sys` (the openssl-sys
//! convention):
//!   1. Locate libsvg via pkg-config. Dev mode links a meson build tree
//!      (`SVG_BUILD_DIR`, else `OPTICS_BUILD_DIR`, else `<checkout>/build`);
//!      installed mode uses the system pkg-config database.
//!   2. Publish the runtime library dirs as `links` metadata so a dependent can
//!      re-emit them (`DEP_SVG_RPATHS`) — `rustc-link-arg` does not cross crate
//!      boundaries. On Windows the DLLs are staged next to the cargo profile
//!      output instead.
//!   3. Run bindgen over `wrapper.h`, preferring `SVG_SOURCE_DIR/include` so the
//!      generated bindings always match the requested checkout.
//!
//! `libsvg` depends on `libflux`, so this script deliberately links *both* and
//! stages both DLLs: a svg consumer that never mentions flux still needs flux
//! present at run time.

use std::env;
use std::path::PathBuf;

fn main() {
    println!("cargo:rerun-if-env-changed=SVG_USE_INSTALLED");
    println!("cargo:rerun-if-env-changed=SVG_BUILD_DIR");
    println!("cargo:rerun-if-env-changed=SVG_SOURCE_DIR");
    println!("cargo:rerun-if-env-changed=FLUX_BUILD_DIR");
    println!("cargo:rerun-if-env-changed=OPTICS_SOURCE_DIR");
    println!("cargo:rerun-if-env-changed=OPTICS_BUILD_DIR");
    println!("cargo:rerun-if-env-changed=PKG_CONFIG_PATH");

    let use_installed = env::var_os("SVG_USE_INSTALLED").is_some();
    let os = target_os();
    let checkout_root = discover_optics_checkout();
    let build_dir = env::var_os("SVG_BUILD_DIR")
        .or_else(|| env::var_os("OPTICS_BUILD_DIR"))
        .map(PathBuf::from)
        .or_else(|| checkout_root.as_ref().map(|root| root.join("build")))
        .filter(|dir| dir.join("meson-uninstalled/svg-uninstalled.pc").exists());
    let source_dir = env::var_os("SVG_SOURCE_DIR")
        .map(PathBuf::from)
        .or_else(|| checkout_root.as_ref().map(|root| root.join("libs/svg")));

    let dev_mode = if let Some(dir) = &build_dir {
        if use_installed {
            false
        } else {
            let pc = dir.join("meson-uninstalled/svg-uninstalled.pc");
            let exists = pc.exists();
            if exists {
                // A build tree that was configured before libsvg existed still
                // has a stale .pc pointing at nothing; probe the library and say
                // so plainly instead of letting pkg-config emit a confusing
                // missing-symbol error later.
                let lib_name = shared_lib_file_name("svg", &os);
                let probe = dir.join("libs/svg").join(&lib_name);
                if !probe.exists() {
                    panic!(
                        "stale or incomplete meson build dir at {}\n\
                         Its `svg-uninstalled.pc` exists but `{lib_name}` is absent.\n\
                         Reconfigure and build the library:\n    \
                         meson setup {0} -Dtests=false && meson compile -C {0}\n\
                         or set FLUX_USE_INSTALLED=1 / SVG_USE_INSTALLED=1 to link an \
                         installed copy.",
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
        let dir = build_dir.as_ref().expect("dev mode implies a build dir");
        // libsvg's own .pc carries `Requires: flux`, so flux's uninstalled dir
        // must be on the search path too when both come from the same tree.
        set_pkg_config_path(&[dir.join("meson-uninstalled")], &os);
    }

    let svg = pkg_config::Config::new()
        .print_system_libs(false)
        .exactly_version(env!("CARGO_PKG_VERSION"))
        .probe("svg")
        .unwrap_or_else(|e| {
            panic!(
                "pkg-config failed for svg: {e}\n\
                 Either:\n  \
                 - install svg (`meson install` into a prefix on PKG_CONFIG_PATH),\n  \
                 - set SVG_BUILD_DIR=<optics>/build to link a meson build tree, or\n  \
                 - set SVG_USE_INSTALLED=1."
            )
        });

    if os == "windows" {
        stage_windows_dlls(&svg.link_paths, &["svg", "flux"]);
    } else {
        let rpaths: Vec<String> = svg
            .link_paths
            .iter()
            .filter(|p| !is_system_libdir(p, &os))
            .map(|p| p.display().to_string())
            .collect();
        emit_rpath_link_args(&rpaths);
        println!("cargo:rpaths={}", rpaths.join(";"));
    }
    let _ = dev_mode;

    let mut clang_args: Vec<String> = svg
        .include_paths
        .iter()
        .map(|p| format!("-I{}", p.display()))
        .collect();
    if let Some(src) = &source_dir {
        clang_args.insert(0, format!("-I{}", src.join("include").display()));
        // svg's public header includes <flux/...>; bindgen needs flux's headers
        // to compile it even though the flux *symbols* are blocklisted below.
        if let Some(root) = &checkout_root {
            clang_args.insert(1, format!("-I{}", root.join("libs/flux/include").display()));
        }
    }

    let bindings = bindgen::Builder::default()
        .rust_edition(bindgen::RustEdition::Edition2024)
        .header("wrapper.h")
        .clang_args(&clang_args)
        .clang_arg("-std=c23")
        .allowlist_function("svg_.*")
        .allowlist_type("svg_.*")
        .allowlist_var("SVG_.*")
        // The flux seam, split the same way lens-sys splits it:
        //  - Opaque handle types are blocklisted and re-exported from flux_sys,
        //    so exactly one Rust definition of each exists across the stack.
        //  - Value types embedded by value in svg structs (flux_color,
        //    flux_point, flux_rect, flux_gradient_stop, the enums) are bound
        //    locally: bindgen cannot derive for structs whose fields reference
        //    blocklisted types, and duplicating POD values carries no seam risk.
        .allowlist_type("flux_color")
        .allowlist_type("flux_point")
        .allowlist_type("flux_rect")
        .allowlist_type("flux_gradient_stop")
        .allowlist_type("flux_gradient_stops")
        .allowlist_type("flux_line_cap")
        .allowlist_type("flux_line_join")
        .allowlist_type("flux_fill_rule")
        .allowlist_type("flux_result")
        .blocklist_type("flux_canvas")
        .blocklist_type("flux_encoder")
        .blocklist_type("flux_device")
        .blocklist_type("flux_image")
        .blocklist_type("flux_path")
        .blocklist_type("flux_arena")
        .blocklist_function("flux_.*")
        .blocklist_var("FLUX_.*")
        .raw_line(
            "pub use flux_sys::{flux_arena, flux_canvas, flux_device, flux_encoder, \
             flux_image, flux_path};",
        )
        .raw_line("pub use flux_sys::flux_color_rgba_premul;")
        .default_enum_style(bindgen::EnumVariation::Rust {
            non_exhaustive: false,
        })
        // svg_path_cmd::verb is a uint8_t holding an svg_path_verb value; the
        // opcode set is extensible (a future verb must not be UB to hold), and
        // svg_pixel_format / svg_paint_kind cross the ABI as plain integers for
        // the same reason.
        .newtype_enum("svg_path_verb")
        .newtype_enum("svg_pixel_format")
        .derive_default(true)
        .derive_debug(true)
        .layout_tests(true)
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
        .generate()
        .expect("bindgen failed to generate svg bindings");

    let out = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR")).join("bindings.rs");
    bindings.write_to_file(&out).expect("write bindings.rs");

    println!("cargo:rerun-if-changed=wrapper.h");
    if let Some(src) = &source_dir {
        println!(
            "cargo:rerun-if-changed={}",
            src.join("include/svg/svg.h").display()
        );
    }
}

fn discover_optics_checkout() -> Option<PathBuf> {
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").ok()?);
    for ancestor in manifest_dir.ancestors() {
        if ancestor.join("libs/svg/include/svg").exists() {
            return Some(ancestor.to_path_buf());
        }
    }
    None
}

fn target_os() -> String {
    env::var("CARGO_CFG_TARGET_OS").unwrap_or_else(|_| "linux".to_string())
}

fn shared_lib_file_name(stem: &str, target_os: &str) -> String {
    match target_os {
        "windows" => format!("{stem}.dll"),
        "macos" => format!("lib{stem}.dylib"),
        _ => format!("lib{stem}.so"),
    }
}

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
    // SAFETY: build scripts are single-threaded at this point and no other
    // thread observes the environment.
    unsafe { env::set_var("PKG_CONFIG_PATH", search) };
}

fn emit_rpath_link_args(rpaths: &[String]) {
    for dir in rpaths {
        println!("cargo:rustc-link-arg=-Wl,-rpath,{dir}");
    }
}

fn stage_windows_dlls(link_dirs: &[PathBuf], stems: &[&str]) {
    let Ok(out_dir) = env::var("OUT_DIR") else {
        return;
    };
    let Some(profile_dir) = PathBuf::from(&out_dir)
        .ancestors()
        .nth(3)
        .map(PathBuf::from)
    else {
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
        let search_dirs = [link_dir.clone(), link_dir.join("../bin")];
        for stem in stems {
            let dll_name = format!("{stem}.dll");
            for search in &search_dirs {
                let src = search.join(&dll_name);
                if src.exists() {
                    for dst_dir in &destinations {
                        let _ = std::fs::copy(&src, dst_dir.join(&dll_name));
                    }
                    break;
                }
            }
        }
    }
}

fn is_system_libdir(path: &std::path::Path, target_os: &str) -> bool {
    if target_os == "windows" {
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
    parts.len() == 1 && parts[0].starts_with("lib")
}
