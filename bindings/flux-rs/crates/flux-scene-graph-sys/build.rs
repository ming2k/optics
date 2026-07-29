//! Build-time discovery and bindgen generation for libflux-scene-graph.

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
        let library = dir.join("libs/flux/scene_graph/libflux_scene_graph.so");
        if !library.exists() {
            panic!(
                "{} is missing; build optics with -Dscene-graph=true first",
                library.display()
            );
        }
        let uninstalled = dir.join("meson-uninstalled");
        let mut search = uninstalled.display().to_string();
        if let Some(existing) = env::var_os("PKG_CONFIG_PATH") {
            search.push(':');
            search.push_str(&existing.to_string_lossy());
        }
        // SAFETY: this build script is single-threaded, and this runs before
        // invoking pkg-config, bindgen, or any other code that may spawn threads.
        unsafe { env::set_var("PKG_CONFIG_PATH", search) };
    }

    let lib = pkg_config::Config::new()
        .print_system_libs(false)
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
    for dir in &rpaths {
        println!("cargo:rustc-link-arg=-Wl,-rpath,{dir}");
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
        if ancestor.join("libs/flux/include/flux").exists()
            && ancestor.join("build/meson-uninstalled").exists()
        {
            return Some(ancestor.to_path_buf());
        }
    }
    None
}
