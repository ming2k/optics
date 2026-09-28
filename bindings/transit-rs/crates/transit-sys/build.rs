//! Build script for `transit-sys`.

use std::env;
use std::path::PathBuf;

fn main() {
    println!("cargo:rerun-if-env-changed=TRANSIT_USE_INSTALLED");
    println!("cargo:rerun-if-env-changed=TRANSIT_BUILD_DIR");
    println!("cargo:rerun-if-env-changed=TRANSIT_SOURCE_DIR");
    println!("cargo:rerun-if-env-changed=PKG_CONFIG_PATH");

    let use_installed = env::var_os("TRANSIT_USE_INSTALLED").is_some();
    let os = target_os();
    let checkout_root = discover_optics_checkout();
    let build_dir = env::var_os("TRANSIT_BUILD_DIR")
        .or_else(|| env::var_os("OPTICS_BUILD_DIR"))
        .map(PathBuf::from)
        .or_else(|| checkout_root.as_ref().map(|root| root.join("build")))
        .filter(|dir| dir.join("meson-uninstalled/transit-uninstalled.pc").exists());
    let source_dir = env::var_os("TRANSIT_SOURCE_DIR")
        .map(PathBuf::from)
        .or_else(|| checkout_root.as_ref().map(|root| root.join("libs/transit")));

    let dev_mode = if let Some(dir) = &build_dir {
        if use_installed {
            false
        } else {
            let uninstalled = dir.join("meson-uninstalled");
            let pc = uninstalled.join("transit-uninstalled.pc");
            let exists = pc.exists();
            if exists {
                let lib_name = shared_lib_file_name("transit", &os);
                let probe = dir.join("libs/transit").join(&lib_name);
                if !probe.exists() {
                    panic!(
                        "stale or incomplete meson build dir at {}\n\
                         Its `transit-uninstalled.pc` exists but `{lib_name}` is absent.",
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

    let lib = pkg_config::Config::new()
        .print_system_libs(false)
        .atleast_version("0.0.30")
        .probe("transit")
        .unwrap_or_else(|e| {
            panic!("pkg-config failed for transit: {e}\nEither install transit or build Optics first.");
        });

    let rpaths: Vec<String> = lib
        .link_paths
        .iter()
        .filter(|p| !is_system_libdir(p, &os))
        .map(|p| p.display().to_string())
        .collect();

    if os == "windows" {
        stage_windows_dlls(&lib.link_paths, &["transit"]);
    } else {
        emit_rpath_link_args(&rpaths);
    }
    println!("cargo:rpaths={}", rpaths.join(";"));

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
        .allowlist_function("transit_.*")
        .allowlist_type("transit_.*")
        .allowlist_var("TRANSIT_.*")
        .default_enum_style(bindgen::EnumVariation::Rust {
            non_exhaustive: false,
        })
        .derive_default(true)
        .derive_debug(true)
        .layout_tests(true)
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
        .generate()
        .expect("bindgen failed to generate transit bindings");

    let out = PathBuf::from(env::var("OUT_DIR").unwrap()).join("bindings.rs");
    bindings.write_to_file(out).expect("write bindings.rs");

    println!("cargo:rerun-if-changed=wrapper.h");
    if let Some(src) = &source_dir {
        println!("cargo:rerun-if-changed={}", src.join("include/transit/transit.h").display());
    }
}

fn discover_optics_checkout() -> Option<PathBuf> {
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").ok()?);
    for ancestor in manifest_dir.ancestors() {
        if ancestor.join("libs/transit/include/transit").exists() {
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
