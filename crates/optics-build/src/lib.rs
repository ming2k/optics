//! Shared build-script helpers for the Optics Rust workspace.
//!
//! Centralizes the Meson uninstalled-tree discovery, `pkg-config` probing,
//! static-preferred Optics archive linking, rpath relay, Windows DLL staging,
//! and `bindgen` defaults so each `optics-*-sys` and wrapper `build.rs` stays
//! declarative and free of duplicated plumbing.

use std::env;
use std::path::{Path, PathBuf};

pub use bindgen;
pub use pkg_config;

const OPTICS_LIBS: &[&str] = &[
    "optics_app",
    "optics_window",
    "optics_ui",
    "optics_material",
    "optics_scene",
    "optics_svg",
    "optics_text",
    "optics_gfx",
];

fn pkg_to_lib_stem(pkg_name: &str) -> String {
    pkg_name.replace('-', "_")
}

fn pkg_to_dir_name(pkg_name: &str) -> &str {
    pkg_name.strip_prefix("optics-").unwrap_or(pkg_name)
}

/// Declarative configuration for probing and generating bindings for one
/// Optics C library.
pub struct SysLibConfig<'a> {
    /// `pkg-config` package and library stem (e.g. `"flux"`, `"lens"`).
    pub pkg_name: &'a str,
    /// Uppercase env-var prefix (e.g. `"FLUX"`, `"LENS"`).
    pub env_prefix: &'a str,
    /// Upstream `<PREFIX>_BUILD_DIR` env prefixes whose `meson-uninstalled/`
    /// directories should also be prepended to `PKG_CONFIG_PATH` in dev mode.
    pub dep_env_prefixes: &'a [&'a str],
    /// Optics library names whose `libs/<name>/include` directories should be
    /// prepended to `bindgen`'s include search path when building against a
    /// source checkout, in priority order.
    pub include_libs: &'a [&'a str],
    /// Additional `pkg-config` packages whose include paths should be passed
    /// to `bindgen` (e.g. `&["vulkan"]` for `optics-flux-sys`).
    pub extra_pkg_config_includes: &'a [&'a str],
    /// Public header filenames under `libs/<pkg_name>/include/<pkg_name>/`
    /// tracked via `cargo:rerun-if-changed`.
    pub headers: &'a [&'a str],
}

/// Output of [`probe_and_link`], ready to configure and run `bindgen`.
pub struct ProbedLib {
    /// Probed `pkg-config` metadata for the target library.
    pub lib: pkg_config::Library,
    /// `-I...` arguments ordered with source-checkout headers first.
    pub clang_args: Vec<String>,
    /// Discovered Optics monorepo root, if building inside or pointed at one.
    pub checkout_root: Option<PathBuf>,
}

impl ProbedLib {
    /// Create a [`bindgen::Builder`] pre-configured with Optics C23 / Rust 2024
    /// conventions, `wrapper.h`, include paths, derives, and Cargo callbacks.
    #[must_use]
    pub fn bindgen_builder(&self) -> bindgen::Builder {
        bindgen::Builder::default()
            .rust_edition(bindgen::RustEdition::Edition2024)
            .header("wrapper.h")
            .clang_args(&self.clang_args)
            .clang_arg("-std=c23")
            .default_enum_style(bindgen::EnumVariation::Rust {
                non_exhaustive: false,
            })
            .derive_default(true)
            .derive_debug(true)
            .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
    }
}

/// Write generated `bindgen` output to `$OUT_DIR/bindings.rs`.
pub fn write_bindings(bindings: &bindgen::Bindings) {
    let out = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR not set")).join("bindings.rs");
    bindings
        .write_to_file(&out)
        .expect("failed to write bindings.rs");
}

/// Probe and emit Cargo link directives for an Optics C library, then return
/// include paths and metadata for `bindgen`.
pub fn probe_and_link(cfg: &SysLibConfig<'_>) -> ProbedLib {
    let use_installed_var = format!("{}_USE_INSTALLED", cfg.env_prefix);
    let build_dir_var = format!("{}_BUILD_DIR", cfg.env_prefix);
    let source_dir_var = format!("{}_SOURCE_DIR", cfg.env_prefix);

    println!("cargo:rerun-if-env-changed={use_installed_var}");
    println!("cargo:rerun-if-env-changed={build_dir_var}");
    println!("cargo:rerun-if-env-changed={source_dir_var}");
    for dep in cfg.dep_env_prefixes {
        println!("cargo:rerun-if-env-changed={dep}_BUILD_DIR");
    }
    println!("cargo:rerun-if-env-changed=OPTICS_SOURCE_DIR");
    println!("cargo:rerun-if-env-changed=OPTICS_BUILD_DIR");
    println!("cargo:rerun-if-env-changed=OPTICS_STATIC");
    println!("cargo:rerun-if-env-changed=OPTICS_DYNAMIC");
    println!("cargo:rerun-if-env-changed=PKG_CONFIG_PATH");

    let use_installed = env::var_os(&use_installed_var).is_some();
    let os = target_os();
    let dir_name = pkg_to_dir_name(cfg.pkg_name);
    let lib_stem = pkg_to_lib_stem(cfg.pkg_name);
    let checkout_root = discover_optics_checkout(dir_name);

    let explicit_build_dir = env::var_os(&build_dir_var).map(PathBuf::from);
    let fallback_build_dir = env::var_os("OPTICS_BUILD_DIR")
        .map(PathBuf::from)
        .or_else(|| checkout_root.as_ref().map(|root| root.join("build")));

    let pc_rel = format!("meson-uninstalled/{}-uninstalled.pc", cfg.pkg_name);

    let dev_build_dir = if use_installed {
        None
    } else if let Some(dir) = explicit_build_dir {
        let pc = dir.join(&pc_rel);
        if !pc.exists() {
            panic!(
                "{build_dir_var} is set to {}\n\
                 but its `{pc_rel}` is missing.\n\
                 Reconfigure with:\n    \
                 meson setup {} -Dtests=false && meson compile -C {}\n\
                 or unset {build_dir_var} to link the system-installed {},\n\
                 or set {use_installed_var}=1.",
                dir.display(),
                dir.display(),
                dir.display(),
                cfg.pkg_name,
            );
        }
        verify_build_artifacts(
            &dir,
            cfg.pkg_name,
            &lib_stem,
            dir_name,
            &os,
            &build_dir_var,
            &use_installed_var,
        );
        Some(dir)
    } else if let Some(dir) = fallback_build_dir {
        if dir.join(&pc_rel).exists() {
            verify_build_artifacts(
                &dir,
                cfg.pkg_name,
                &lib_stem,
                dir_name,
                &os,
                &build_dir_var,
                &use_installed_var,
            );
            Some(dir)
        } else {
            None
        }
    } else {
        None
    };

    if let Some(dir) = &dev_build_dir {
        let mut dirs = vec![dir.join("meson-uninstalled")];
        for dep in cfg.dep_env_prefixes {
            let var = format!("{dep}_BUILD_DIR");
            let other = env::var_os(&var)
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

    let lib = probe_and_emit_link(cfg.pkg_name, &lib_stem, &os, &use_installed_var);

    let mut clang_args: Vec<String> = lib
        .include_paths
        .iter()
        .map(|p| format!("-I{}", p.display()))
        .collect();

    let explicit_source = env::var_os(&source_dir_var).map(PathBuf::from);
    let mut header_base: Option<PathBuf> = None;

    if let Some(src) = &explicit_source {
        if src.join("include").is_dir() {
            // Points directly at libs/<dir_name>
            let mut prepend = vec![format!("-I{}", src.join("include").display())];
            if let Some(root) = &checkout_root {
                for &dep_lib in cfg.include_libs {
                    if dep_lib != dir_name {
                        prepend.push(format!(
                            "-I{}",
                            root.join(format!("libs/{dep_lib}/include")).display()
                        ));
                    }
                }
            }
            prepend.extend(clang_args);
            clang_args = prepend;
            header_base = Some(src.join("include").join("optics"));
        } else {
            // Points at the Optics repository root
            let mut prepend = Vec::with_capacity(cfg.include_libs.len());
            for &inc_lib in cfg.include_libs {
                prepend.push(format!(
                    "-I{}",
                    src.join(format!("libs/{inc_lib}/include")).display()
                ));
            }
            prepend.extend(clang_args);
            clang_args = prepend;
            header_base = Some(src.join(format!("libs/{dir_name}/include/optics")));
        }
    } else if let Some(root) = &checkout_root {
        let mut prepend = Vec::with_capacity(cfg.include_libs.len());
        for &inc_lib in cfg.include_libs {
            prepend.push(format!(
                "-I{}",
                root.join(format!("libs/{inc_lib}/include")).display()
            ));
        }
        prepend.extend(clang_args);
        clang_args = prepend;
        header_base = Some(root.join(format!("libs/{dir_name}/include/optics")));
    }

    for extra_pkg in cfg.extra_pkg_config_includes {
        if let Ok(extra) = pkg_config::Config::new()
            .print_system_libs(false)
            .probe(extra_pkg)
        {
            for p in &extra.include_paths {
                clang_args.push(format!("-I{}", p.display()));
            }
        }
    }

    println!("cargo:rerun-if-changed=wrapper.h");
    if let Some(base) = header_base {
        for h in cfg.headers {
            println!("cargo:rerun-if-changed={}", base.join(h).display());
        }
    }

    ProbedLib {
        lib,
        clang_args,
        checkout_root,
    }
}

/// Relay `rpath` link arguments published by upstream `-sys` or wrapper crates
/// so this crate's binaries (tests and examples) find locally built shared
/// libraries without `LD_LIBRARY_PATH`.
///
/// When `republish` is `true`, emits `cargo:rpaths=...` so downstream crates
/// depending on this crate (via its `links` key) can relay the same set.
pub fn relay_rpaths(dep_vars: &[&str], republish: bool) {
    let os = target_os();
    if os == "windows" {
        if republish {
            println!("cargo:rpaths=");
        }
        return;
    }

    let values: Vec<String> = dep_vars
        .iter()
        .filter_map(|var| env::var(var).ok())
        .collect();
    let dirs = dedup_rpaths(&values);

    if !dirs.is_empty() {
        // Old-style DT_RPATH (not DT_RUNPATH) covers transitive shared-library
        // dependencies. GNU-style ELF linkers only; macOS ld64 rejects this flag.
        if os != "macos" {
            println!("cargo:rustc-link-arg=-Wl,--disable-new-dtags");
        }
        for dir in &dirs {
            println!("cargo:rustc-link-arg=-Wl,-rpath,{dir}");
        }
    }

    if republish {
        println!("cargo:rpaths={}", dirs.join(";"));
    }
}

fn verify_build_artifacts(
    dir: &Path,
    pkg_name: &str,
    lib_stem: &str,
    dir_name: &str,
    os: &str,
    build_dir_var: &str,
    use_installed_var: &str,
) {
    let static_name = static_lib_file_name(lib_stem, os);
    let shared_name = shared_lib_file_name(lib_stem, os);
    let sub = format!("libs/{dir_name}");
    let found = [&static_name, &shared_name]
        .iter()
        .any(|name| dir.join(name).exists() || dir.join(&sub).join(name).exists());
    if !found {
        panic!(
            "stale or incomplete meson build dir at {}\n\
             Its `{pkg_name}-uninstalled.pc` exists but neither `{static_name}` nor `{shared_name}` is \
             present — either `meson compile` has not run yet, or the \
             build tree no longer matches the source checkout.\n\
             Reconfigure with:\n    \
             meson setup {} -Dtests=false && meson compile -C {}\n\
             or set {build_dir_var} to a fresh build directory,\n\
             or set {use_installed_var}=1 to link the system-installed {pkg_name}.",
            dir.display(),
            dir.display(),
            dir.display(),
        );
    }
}

fn discover_optics_checkout(component: &str) -> Option<PathBuf> {
    let marker = format!("libs/{component}/include/optics");
    if let Some(root) = env::var_os("OPTICS_SOURCE_DIR") {
        let root = PathBuf::from(root);
        if root.join(&marker).exists() {
            return Some(root);
        }
    }
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").ok()?);
    for ancestor in manifest_dir.ancestors() {
        if ancestor.join(&marker).exists() {
            return Some(ancestor.to_path_buf());
        }
    }
    None
}

fn target_os() -> String {
    env::var("CARGO_CFG_TARGET_OS").unwrap_or_else(|_| "linux".to_string())
}

fn shared_lib_file_name(stem: &str, os: &str) -> String {
    match os {
        "windows" => format!("{stem}.dll"),
        "macos" => format!("lib{stem}.dylib"),
        _ => format!("lib{stem}.so"),
    }
}

fn static_lib_file_name(stem: &str, os: &str) -> String {
    match os {
        "windows" => format!("{stem}.lib"),
        _ => format!("lib{stem}.a"),
    }
}

fn find_static_archive(search_dirs: &[PathBuf], stem: &str, os: &str) -> Option<PathBuf> {
    let primary = static_lib_file_name(stem, os);
    for dir in search_dirs {
        let p = dir.join(&primary);
        if p.is_file() {
            return Some(p);
        }
        if os == "windows" {
            let mingw = dir.join(format!("lib{stem}.a"));
            if mingw.is_file() {
                return Some(mingw);
            }
        }
    }
    None
}

fn archive_search_dirs(pkg_name: &str, link_paths: &[PathBuf]) -> Vec<PathBuf> {
    let mut dirs = link_paths.to_vec();
    if let Ok(libdir) = pkg_config::get_variable(pkg_name, "libdir") {
        let dir = PathBuf::from(libdir);
        if !dirs.contains(&dir) {
            dirs.push(dir);
        }
    }
    dirs
}

/// True for loader-default library directories (`/lib`, `/lib64`, `/usr/lib`,
/// `/usr/lib64`, `/usr/lib/<triplet>`, etc.) that never need an rpath.
/// Homebrew's `/usr/local/lib` is deliberately NOT filtered.
fn is_system_libdir(path: &Path, os: &str) -> bool {
    if os == "windows" {
        return false;
    }
    let resolved = path.canonicalize().unwrap_or_else(|_| path.to_path_buf());
    let text = resolved.to_string_lossy();
    let parts: Vec<&str> = text.split('/').filter(|s| !s.is_empty()).collect();
    if parts.len() >= 2
        && (parts[0] == "usr" || parts[0] == "lib")
        && (parts[1] == "lib" || parts[1] == "lib64" || parts[1] == "lib32")
    {
        return true;
    }
    if parts.len() == 1 && parts[0].starts_with("lib") {
        return true;
    }
    false
}

fn dedup_rpaths(raw_lists: &[String]) -> Vec<String> {
    let mut out = Vec::new();
    for raw in raw_lists {
        for dir in raw.split(';').filter(|s| !s.is_empty()) {
            let dir = dir.to_owned();
            if !out.contains(&dir) {
                out.push(dir);
            }
        }
    }
    out
}

fn probe_and_emit_link(
    pkg_name: &str,
    lib_stem: &str,
    os: &str,
    use_installed_var: &str,
) -> pkg_config::Library {
    let require_static = env::var_os("OPTICS_STATIC").is_some();
    let force_dynamic = env::var_os("OPTICS_DYNAMIC").is_some();
    let pkg_version = env::var("CARGO_PKG_VERSION").expect("CARGO_PKG_VERSION not set");

    let base = pkg_config::Config::new()
        .cargo_metadata(false)
        .print_system_libs(false)
        .exactly_version(&pkg_version)
        .probe(pkg_name)
        .unwrap_or_else(|e| {
            panic!(
                "pkg-config failed for {pkg_name}: {e}\n\
                 Either:\n  \
                 - build Optics (`meson setup build -Dtests=false && meson compile -C build`),\n  \
                 - install {pkg_name} (`meson install` into a prefix on PKG_CONFIG_PATH),\n  \
                 - set OPTICS_BUILD_DIR=<optics>/build to link a meson build tree, or\n  \
                 - set {use_installed_var}=1 to skip the build-tree probe.",
            )
        });

    let search_dirs = archive_search_dirs(pkg_name, &base.link_paths);
    let has_static = find_static_archive(&search_dirs, lib_stem, os).is_some();
    let has_shared = search_dirs
        .iter()
        .any(|dir| dir.join(shared_lib_file_name(lib_stem, os)).exists());
    let use_static = !force_dynamic && has_static;

    if require_static && !use_static {
        panic!(
            "OPTICS_STATIC is set, but no static archive `{}` was found for `{pkg_name}` in {:?}.\n\
             Rebuild Optics with `-Ddefault_library=static`.",
            static_lib_file_name(lib_stem, os),
            search_dirs,
        );
    }

    let lib = if use_static && has_shared {
        pkg_config::Config::new()
            .cargo_metadata(false)
            .statik(true)
            .print_system_libs(false)
            .exactly_version(&pkg_version)
            .probe(pkg_name)
            .unwrap_or(base)
    } else {
        base
    };

    for dir in &lib.link_paths {
        println!("cargo:rustc-link-search=native={}", dir.display());
    }
    for dir in &lib.framework_paths {
        println!("cargo:rustc-link-search=framework={}", dir.display());
    }

    if use_static {
        let mut all_archive_dirs = archive_search_dirs(pkg_name, &lib.link_paths);
        for name in &lib.libs {
            if OPTICS_LIBS.contains(&name.as_str()) {
                let pc_name = name.replace('_', "-");
                if let Ok(libdir) = pkg_config::get_variable(&pc_name, "libdir") {
                    let dir = PathBuf::from(libdir);
                    if !all_archive_dirs.contains(&dir) {
                        all_archive_dirs.push(dir);
                    }
                }
                if let Some(archive) = find_static_archive(&all_archive_dirs, name, os) {
                    println!("cargo:rerun-if-changed={}", archive.display());
                    println!("cargo:rustc-link-lib=static:-bundle={name}");
                } else {
                    panic!(
                        "static archive `{}` for Optics library `{name}` not found in {:?}; \
                         rebuild Optics with `-Ddefault_library=static` (or set OPTICS_DYNAMIC=1).",
                        static_lib_file_name(name, os),
                        all_archive_dirs,
                    );
                }
            } else {
                println!("cargo:rustc-link-lib={name}");
            }
        }
    } else {
        for name in &lib.libs {
            println!("cargo:rustc-link-lib={name}");
        }
    }

    for fw in &lib.frameworks {
        println!("cargo:rustc-link-lib=framework={fw}");
    }

    let non_system_dirs: Vec<String> = lib
        .link_paths
        .iter()
        .filter(|p| !is_system_libdir(p, os))
        .map(|p| p.display().to_string())
        .collect();
    println!("cargo:link_paths={}", non_system_dirs.join(";"));

    if use_static {
        println!("cargo:rpaths=");
    } else {
        if os == "windows" {
            let optics_present: Vec<&str> = lib
                .libs
                .iter()
                .filter_map(|l| OPTICS_LIBS.iter().copied().find(|&o| o == l.as_str()))
                .collect();
            stage_windows_dlls(&lib.link_paths, &optics_present);
        } else {
            for dir in &non_system_dirs {
                println!("cargo:rustc-link-arg=-Wl,-rpath,{dir}");
            }
        }
        println!("cargo:rpaths={}", non_system_dirs.join(";"));
    }

    lib
}

fn set_pkg_config_path(dirs: &[PathBuf], os: &str) {
    let sep = if os == "windows" { ';' } else { ':' };
    let mut search = dirs
        .iter()
        .map(|d| d.display().to_string())
        .collect::<Vec<_>>()
        .join(&sep.to_string());
    if let Some(existing) = env::var_os("PKG_CONFIG_PATH") {
        search.push(sep);
        search.push_str(&existing.to_string_lossy());
    }
    // SAFETY: build scripts run single-threaded before invoking pkg-config or
    // bindgen.
    unsafe { env::set_var("PKG_CONFIG_PATH", search) };
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
        let search_dirs = [link_dir.clone(), link_dir.join("../bin")];
        for stem in stems {
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn library_file_names_match_target_os() {
        assert_eq!(shared_lib_file_name("flux", "linux"), "libflux.so");
        assert_eq!(shared_lib_file_name("flux", "macos"), "libflux.dylib");
        assert_eq!(shared_lib_file_name("flux", "windows"), "flux.dll");

        assert_eq!(static_lib_file_name("flux", "linux"), "libflux.a");
        assert_eq!(static_lib_file_name("flux", "macos"), "libflux.a");
        assert_eq!(static_lib_file_name("flux", "windows"), "flux.lib");
    }

    #[test]
    fn system_libdir_filters_standard_unix_dirs_only() {
        assert!(is_system_libdir(Path::new("/usr/lib"), "linux"));
        assert!(is_system_libdir(Path::new("/usr/lib64"), "linux"));
        assert!(is_system_libdir(
            Path::new("/usr/lib/x86_64-linux-gnu"),
            "linux"
        ));
        assert!(is_system_libdir(Path::new("/lib"), "linux"));
        assert!(!is_system_libdir(Path::new("/usr/local/lib"), "linux"));
        assert!(!is_system_libdir(Path::new("/opt/optics/lib"), "macos"));
        assert!(!is_system_libdir(Path::new("/usr/lib"), "windows"));
    }

    #[test]
    fn dedup_rpaths_preserves_order_and_drops_empty_segments() {
        let input = vec!["/a;/b;;/a".to_string(), "".to_string(), "/b;/c".to_string()];
        assert_eq!(dedup_rpaths(&input), vec!["/a", "/b", "/c"]);
    }
}
