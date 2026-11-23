//! Shared build-script helpers for the Optics Rust workspace.
//!
//! Centralizes the Meson uninstalled-tree discovery, `pkg-config` probing,
//! shared-or-static Optics linking, rpath relay, Windows DLL staging,
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
    /// `pkg-config` package and library stem (e.g. `"gfx"`, `"ui"`).
    pub pkg_name: &'a str,
    /// Uppercase env-var prefix (e.g. `"GFX"`, `"UI"`).
    pub env_prefix: &'a str,
    /// Upstream `<PREFIX>_BUILD_DIR` env prefixes whose `meson-uninstalled/`
    /// directories should also be prepended to `PKG_CONFIG_PATH` in dev mode.
    pub dep_env_prefixes: &'a [&'a str],
    /// Optics library names whose `libs/<name>/include` directories should be
    /// prepended to `bindgen`'s include search path when building against a
    /// source checkout, in priority order.
    pub include_libs: &'a [&'a str],
    /// Additional `pkg-config` packages whose include paths should be passed
    /// to `bindgen` (e.g. `&["vulkan"]` for `optics-gfx-sys`).
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
    println!("cargo:rerun-if-env-changed=PKG_CONFIG_ALL_STATIC");
    println!("cargo:rerun-if-env-changed=PKG_CONFIG_ALL_DYNAMIC");
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

    let lib = probe_and_emit_link(
        cfg.pkg_name,
        &lib_stem,
        &os,
        &use_installed_var,
        cfg.dep_env_prefixes,
    );

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

/// Static archive name. Meson uses `lib<stem>.a` on every platform, Windows
/// included, because `<stem>.lib` is already the DLL's import library.
fn static_lib_file_name(stem: &str, _os: &str) -> String {
    format!("lib{stem}.a")
}

/// Files that let the linker resolve `-l<stem>` against the shared library:
/// the shared object itself on Unix, the import library on Windows (installed
/// DLLs live in `bin/`, not in the pkg-config `libdir`).
fn shared_link_file_names(stem: &str, os: &str) -> Vec<String> {
    match os {
        "windows" => vec![format!("{stem}.lib"), format!("lib{stem}.dll.a")],
        _ => vec![shared_lib_file_name(stem, os)],
    }
}

fn has_shared_link_artifact(search_dirs: &[PathBuf], stem: &str, os: &str) -> bool {
    let names = shared_link_file_names(stem, os);
    search_dirs
        .iter()
        .any(|dir| names.iter().any(|name| dir.join(name).is_file()))
}

fn find_static_archive(search_dirs: &[PathBuf], stem: &str, os: &str) -> Option<PathBuf> {
    let name = static_lib_file_name(stem, os);
    search_dirs
        .iter()
        .map(|dir| dir.join(&name))
        .find(|path| path.is_file())
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

/// How the caller wants Optics linked.
///
/// The default follows the usual `-sys` convention: link the shared objects
/// the host provides, and embed the archives only on request. Only
/// `DynamicPreferred` looks at the filesystem before deciding.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum LinkMode {
    /// Use the shared objects when they exist; use the archives only on a
    /// static-only tree.
    DynamicPreferred,
    /// Require the archives; fail when the host cannot provide a static link.
    Static,
    /// Require the shared objects; fail when the host has none.
    Dynamic,
}

/// The link a `-sys` crate actually emitted, published to dependents as
/// `cargo:link_mode=` and read back as `DEP_<LINKS>_LINK_MODE`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Linkage {
    Static,
    Dynamic,
}

impl Linkage {
    fn as_str(self) -> &'static str {
        match self {
            Self::Static => "static",
            Self::Dynamic => "dynamic",
        }
    }

    fn parse(value: &str) -> Option<Self> {
        match value {
            "static" => Some(Self::Static),
            "dynamic" => Some(Self::Dynamic),
            _ => None,
        }
    }
}

/// A link-mode request together with the switch that made it, so diagnostics
/// can name what to change.
#[derive(Clone, Debug, PartialEq, Eq)]
struct LinkRequest {
    mode: LinkMode,
    origin: Option<String>,
}

/// Read the link-mode switches.
///
/// The environment variables come first: they are the decision of whoever runs
/// the build (a packager, CI) and can force either mode. Below them sits the
/// single additive `static` Cargo feature. There is deliberately no `dynamic`
/// feature: Cargo unifies features across the graph, so a pair of mutually
/// exclusive features would let two unrelated dependents break each other's
/// build. Shared linking is the default, so it needs no feature.
///
/// `PKG_CONFIG_ALL_STATIC` / `PKG_CONFIG_ALL_DYNAMIC` are honoured as aliases:
/// the `pkg-config` crate infers the mode from them, so ignoring them here
/// would let a caller believe it controlled the link while the emitted
/// directives came from somewhere else. Same-direction duplicates are harmless
/// and the `OPTICS_*` spelling is the one named in diagnostics; one variable of
/// each direction (including a mixed pair such as `OPTICS_STATIC` +
/// `PKG_CONFIG_ALL_DYNAMIC`) is a hard error instead of an arbitrary winner.
fn requested_link_mode() -> LinkRequest {
    fn first_set<'a>(vars: &[&'a str]) -> Option<&'a str> {
        vars.iter().find(|var| env::var_os(var).is_some()).copied()
    }
    select_link_mode(
        first_set(&["OPTICS_STATIC", "PKG_CONFIG_ALL_STATIC"]),
        first_set(&["OPTICS_DYNAMIC", "PKG_CONFIG_ALL_DYNAMIC"]),
        env::var_os("CARGO_FEATURE_STATIC").is_some(),
    )
}

/// Resolve the link mode from the switches that were set.
///
/// Split from [`requested_link_mode`] so the decision is testable without
/// mutating the process environment.
fn select_link_mode(
    env_static: Option<&str>,
    env_dynamic: Option<&str>,
    static_feature: bool,
) -> LinkRequest {
    let (mode, origin) = match (env_static, env_dynamic) {
        (Some(statik), Some(dynamic)) => panic!(
            "{statik} and {dynamic} request opposite Optics link modes.\n\
             Unset one of them: `{statik}` embeds the liboptics_*.a archives, \
             `{dynamic}` links the liboptics_*.so objects."
        ),
        (Some(origin), None) => (LinkMode::Static, Some(origin)),
        (None, Some(origin)) => (LinkMode::Dynamic, Some(origin)),
        (None, None) if static_feature => (LinkMode::Static, Some("the `static` Cargo feature")),
        (None, None) => (LinkMode::DynamicPreferred, None),
    };
    LinkRequest {
        mode,
        origin: origin.map(str::to_owned),
    }
}

/// Read the linkage the upstream Optics `-sys` crates already emitted.
///
/// Every Optics `-sys` crate in one build must link the same way. A static
/// `liboptics_gfx.a` next to a shared `liboptics_ui.so` (which itself needs
/// `liboptics_gfx.so`) puts two copies of gfx — two sets of globals — into one
/// process. The upstream decision is therefore binding on dependents.
fn upstream_linkage(dep_env_prefixes: &[&str]) -> Option<(String, Linkage)> {
    let mut found: Option<(String, Linkage)> = None;
    for dep in dep_env_prefixes {
        let var = format!("DEP_{dep}_LINK_MODE");
        let Ok(value) = env::var(&var) else {
            continue;
        };
        let linkage = Linkage::parse(&value)
            .unwrap_or_else(|| panic!("{var}={value} is neither `static` nor `dynamic`"));
        let krate = format!("{}-sys", dep.to_lowercase().replace('_', "-"));
        match &found {
            Some((other, prior)) if *prior != linkage => panic!(
                "{other} linked Optics {} but {krate} linked it {}; one build must \
                 link every Optics module the same way. Set OPTICS_STATIC=1 or \
                 OPTICS_DYNAMIC=1 for the whole build.",
                prior.as_str(),
                linkage.as_str(),
            ),
            Some(_) => {}
            None => found = Some((krate, linkage)),
        }
    }
    found
}

/// Decide the linkage for `pkg_name`; the pure core of [`probe_and_emit_link`].
///
/// `static_ready` is only called when the decision depends on it, so a
/// shared link never runs the `pkg-config --static` probe (whose private
/// dependencies may legitimately be absent). Returns the linkage, or the
/// panic message.
fn resolve_linkage(
    pkg_name: &str,
    search_dirs: &str,
    request: &LinkRequest,
    upstream: Option<&(String, Linkage)>,
    has_shared: bool,
    static_ready: impl FnOnce() -> Result<(), String>,
) -> Result<Linkage, String> {
    let (mode, why) = match (request.mode, upstream) {
        (LinkMode::DynamicPreferred, Some((dep, linkage))) => (
            match linkage {
                Linkage::Static => LinkMode::Static,
                Linkage::Dynamic => LinkMode::Dynamic,
            },
            format!("to match {dep}, which linked Optics {}", linkage.as_str()),
        ),
        (LinkMode::Static, Some((dep, Linkage::Dynamic)))
        | (LinkMode::Dynamic, Some((dep, Linkage::Static))) => {
            let origin = request.origin.as_deref().unwrap_or("the request");
            let (wanted, got) = if request.mode == LinkMode::Static {
                ("static", "dynamic")
            } else {
                ("dynamic", "static")
            };
            return Err(format!(
                "{origin} asks for a {wanted} link of `{pkg_name}`, but {dep} \
                 already linked Optics {got}; one build must link every Optics \
                 module the same way.\n\
                 Enable the `static` feature on the `optics` crate (it reaches \
                 every module), or set OPTICS_STATIC=1 / OPTICS_DYNAMIC=1 for \
                 the whole build."
            ));
        }
        (mode, _) => (mode, request.origin.clone().unwrap_or_default()),
    };
    // Whether `mode` is the caller's own request (which they can drop) or was
    // adopted from upstream (which only a whole-build override changes).
    let requested = request.mode == mode;

    let no_shared = || {
        let alternative = if requested {
            "or drop the dynamic request to link the archives"
        } else {
            "or set OPTICS_STATIC=1 to link every Optics module statically"
        };
        format!(
            "Optics was asked to link `{pkg_name}` dynamically ({why}), but no \
             shared library was found in {search_dirs}.\n\
             `-l` would silently resolve to the archive, so this is refused. \
             Rebuild Optics with the shared objects (`meson setup build \
             -Ddefault_library=both` or `=shared`), {alternative}."
        )
    };

    match mode {
        LinkMode::Static => static_ready().map(|()| Linkage::Static).map_err(|detail| {
            let alternative = if requested {
                "Or drop the static request (the `static` feature or OPTICS_STATIC) \
                 to link the shared objects."
            } else {
                "Or set OPTICS_DYNAMIC=1 to link every Optics module shared."
            };
            format!(
                "Optics was asked to link `{pkg_name}` statically ({why}), but {detail}\n\
                 {alternative}"
            )
        }),
        LinkMode::Dynamic if has_shared => Ok(Linkage::Dynamic),
        LinkMode::Dynamic => Err(no_shared()),
        LinkMode::DynamicPreferred if has_shared => Ok(Linkage::Dynamic),
        // A static-only tree: the archives are the only thing to link.
        LinkMode::DynamicPreferred => static_ready().map(|()| Linkage::Static).map_err(|detail| {
            format!(
                "`{pkg_name}` can be linked neither dynamically nor statically: \
                 no shared library was found in {search_dirs}, and {detail}"
            )
        }),
    }
}

/// Locate the archive of every Optics module in `lib`'s static closure.
///
/// A static link has to embed the whole closure; a tree that has archives for
/// some modules but not others cannot produce one.
fn closure_archives(
    pkg_name: &str,
    lib: &pkg_config::Library,
    os: &str,
) -> Result<Vec<(String, PathBuf)>, String> {
    let mut dirs = archive_search_dirs(pkg_name, &lib.link_paths);
    let mut archives = Vec::new();
    for name in lib
        .libs
        .iter()
        .filter(|n| OPTICS_LIBS.contains(&n.as_str()))
    {
        if let Ok(libdir) = pkg_config::get_variable(&name.replace('_', "-"), "libdir") {
            let dir = PathBuf::from(libdir);
            if !dirs.contains(&dir) {
                dirs.push(dir);
            }
        }
        let archive = find_static_archive(&dirs, name, os).ok_or_else(|| {
            format!(
                "the static closure also needs `{}`, which is missing from {dirs:?}. \
                 Rebuild Optics so every module has its archive \
                 (`meson setup build -Ddefault_library=both`).",
                static_lib_file_name(name, os),
            )
        })?;
        archives.push((name.clone(), archive));
    }
    Ok(archives)
}

fn probe_and_emit_link(
    pkg_name: &str,
    lib_stem: &str,
    os: &str,
    use_installed_var: &str,
    dep_env_prefixes: &[&str],
) -> pkg_config::Library {
    let request = requested_link_mode();
    let upstream = upstream_linkage(dep_env_prefixes);
    let pkg_version = env::var("CARGO_PKG_VERSION").expect("CARGO_PKG_VERSION not set");

    // The mode is always explicit: relying on `pkg-config`'s env inference made
    // the emitted directives depend on variables this function never read.
    let probe = |statik: bool| {
        pkg_config::Config::new()
            .cargo_metadata(false)
            .statik(statik)
            .print_system_libs(false)
            .exactly_version(&pkg_version)
            .probe(pkg_name)
    };

    let base = probe(false).unwrap_or_else(|e| {
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
    let has_shared = has_shared_link_artifact(&search_dirs, lib_stem, os);

    // The `--static` probe is the only source of `Requires.private` (vulkan,
    // wayland-client, fontconfig, ...); without it the link fails with
    // undefined symbols. It runs only when the decision needs it.
    let mut static_link: Option<(pkg_config::Library, Vec<(String, PathBuf)>)> = None;
    let decision = resolve_linkage(
        pkg_name,
        &format!("{search_dirs:?}"),
        &request,
        upstream.as_ref(),
        has_shared,
        || {
            if find_static_archive(&search_dirs, lib_stem, os).is_none() {
                return Err(format!(
                    "no `{}` was found in {search_dirs:?}.\n\
                     Rebuild Optics so the archives exist: `meson setup build \
                     -Ddefault_library=both` (the upstream default; `=static` also works).",
                    static_lib_file_name(lib_stem, os),
                ));
            }
            let lib = probe(true).map_err(|e| {
                format!(
                    "the `pkg-config --static` probe, which supplies the module's \
                     `Requires.private` dependencies, failed: {e}\n\
                     Install the development packages of those dependencies \
                     (vulkan, wayland-client, xkbcommon, fontconfig, freetype2, \
                     harfbuzz, ...).",
                )
            })?;
            let archives = closure_archives(pkg_name, &lib, os)?;
            static_link = Some((lib, archives));
            Ok(())
        },
    );
    let linkage = decision.unwrap_or_else(|message| panic!("{message}"));
    println!("cargo:link_mode={}", linkage.as_str());

    let lib = match linkage {
        Linkage::Static => {
            let (lib, archives) = static_link.expect("static linkage implies a static probe");
            emit_search_paths(&lib);
            for name in &lib.libs {
                match archives.iter().find(|(stem, _)| stem == name) {
                    Some((_, archive)) => {
                        println!("cargo:rerun-if-changed={}", archive.display());
                        if os == "windows" {
                            // Meson names Windows archives `lib<stem>.a`; a plain
                            // `-l<stem>` would resolve `<stem>.lib`, which is the
                            // DLL's import library.
                            let file = archive.file_name().unwrap().to_string_lossy();
                            println!("cargo:rustc-link-lib=static:-bundle,+verbatim={file}");
                        } else {
                            println!("cargo:rustc-link-lib=static:-bundle={name}");
                        }
                    }
                    None => println!("cargo:rustc-link-lib={name}"),
                }
            }
            lib
        }
        Linkage::Dynamic => {
            emit_search_paths(&base);
            for name in &base.libs {
                println!("cargo:rustc-link-lib={name}");
            }
            base
        }
    };

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

    match linkage {
        // Nothing has to be found at runtime.
        Linkage::Static => println!("cargo:rpaths="),
        Linkage::Dynamic => {
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
    }

    lib
}

fn emit_search_paths(lib: &pkg_config::Library) {
    for dir in &lib.link_paths {
        println!("cargo:rustc-link-search=native={}", dir.display());
    }
    for dir in &lib.framework_paths {
        println!("cargo:rustc-link-search=framework={}", dir.display());
    }
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
        assert_eq!(
            shared_lib_file_name("optics_gfx", "linux"),
            "liboptics_gfx.so"
        );
        assert_eq!(
            shared_lib_file_name("optics_gfx", "macos"),
            "liboptics_gfx.dylib"
        );
        assert_eq!(
            shared_lib_file_name("optics_gfx", "windows"),
            "optics_gfx.dll"
        );

        assert_eq!(
            static_lib_file_name("optics_gfx", "linux"),
            "liboptics_gfx.a"
        );
        assert_eq!(
            static_lib_file_name("optics_gfx", "macos"),
            "liboptics_gfx.a"
        );
        assert_eq!(
            static_lib_file_name("optics_gfx", "windows"),
            "liboptics_gfx.a"
        );
        assert_eq!(
            shared_link_file_names("optics_gfx", "windows"),
            vec!["optics_gfx.lib", "liboptics_gfx.dll.a"]
        );
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

    fn mode(env_static: Option<&str>, env_dynamic: Option<&str>, feature: bool) -> LinkMode {
        select_link_mode(env_static, env_dynamic, feature).mode
    }

    #[test]
    fn link_mode_defaults_to_dynamic_preferred() {
        assert_eq!(mode(None, None, false), LinkMode::DynamicPreferred);
    }

    #[test]
    fn static_feature_forces_static_and_is_named() {
        let request = select_link_mode(None, None, true);
        assert_eq!(request.mode, LinkMode::Static);
        assert_eq!(
            request.origin.as_deref(),
            Some("the `static` Cargo feature")
        );
    }

    #[test]
    fn environment_overrides_the_feature_in_both_directions() {
        assert_eq!(mode(Some("OPTICS_STATIC"), None, false), LinkMode::Static);
        assert_eq!(
            mode(Some("PKG_CONFIG_ALL_STATIC"), None, false),
            LinkMode::Static
        );
        assert_eq!(mode(None, Some("OPTICS_DYNAMIC"), false), LinkMode::Dynamic);
        assert_eq!(
            mode(None, Some("PKG_CONFIG_ALL_DYNAMIC"), false),
            LinkMode::Dynamic
        );
        // A packager can force a shared link over a dependent's `static` feature.
        assert_eq!(mode(None, Some("OPTICS_DYNAMIC"), true), LinkMode::Dynamic);
        assert_eq!(mode(Some("OPTICS_STATIC"), None, true), LinkMode::Static);
    }

    #[test]
    fn contradictory_environment_is_rejected() {
        let conflict = std::panic::catch_unwind(|| {
            select_link_mode(Some("OPTICS_STATIC"), Some("OPTICS_DYNAMIC"), false)
        });
        let message = *conflict
            .expect_err("a contradictory request must not resolve to a mode")
            .downcast::<String>()
            .expect("panic message is a String");
        assert!(message.contains("OPTICS_STATIC"), "{message}");
        assert!(message.contains("OPTICS_DYNAMIC"), "{message}");
    }

    fn request(mode: LinkMode) -> LinkRequest {
        LinkRequest {
            mode,
            origin: (mode != LinkMode::DynamicPreferred).then(|| "TEST_SWITCH".to_owned()),
        }
    }

    fn ready() -> Result<(), String> {
        Ok(())
    }

    fn unavailable() -> Result<(), String> {
        Err("no archive".to_owned())
    }

    fn untouched() -> Result<(), String> {
        panic!("the static probe must not run for a shared link")
    }

    fn resolve(
        mode: LinkMode,
        upstream: Option<Linkage>,
        has_shared: bool,
        static_ready: fn() -> Result<(), String>,
    ) -> Result<Linkage, String> {
        let upstream = upstream.map(|l| ("optics-gfx-sys".to_owned(), l));
        resolve_linkage(
            "optics-ui",
            "[dirs]",
            &request(mode),
            upstream.as_ref(),
            has_shared,
            static_ready,
        )
    }

    #[test]
    fn default_prefers_shared_objects_even_when_archives_exist() {
        assert_eq!(
            resolve(LinkMode::DynamicPreferred, None, true, untouched),
            Ok(Linkage::Dynamic)
        );
    }

    #[test]
    fn default_uses_archives_only_on_a_static_only_tree() {
        assert_eq!(
            resolve(LinkMode::DynamicPreferred, None, false, ready),
            Ok(Linkage::Static)
        );
        let err = resolve(LinkMode::DynamicPreferred, None, false, unavailable).unwrap_err();
        assert!(err.contains("neither dynamically nor statically"), "{err}");
    }

    #[test]
    fn explicit_modes_never_degrade() {
        assert_eq!(
            resolve(LinkMode::Static, None, true, ready),
            Ok(Linkage::Static)
        );
        let err = resolve(LinkMode::Static, None, true, unavailable).unwrap_err();
        assert!(
            err.contains("TEST_SWITCH") && err.contains("no archive"),
            "{err}"
        );

        assert_eq!(
            resolve(LinkMode::Dynamic, None, true, untouched),
            Ok(Linkage::Dynamic)
        );
        let err = resolve(LinkMode::Dynamic, None, false, untouched).unwrap_err();
        assert!(err.contains("dynamically"), "{err}");
    }

    #[test]
    fn upstream_linkage_binds_default_dependents() {
        // gfx went static (a static-only gfx, or a request on gfx alone): a
        // dependent must follow even though it has shared objects.
        assert_eq!(
            resolve(
                LinkMode::DynamicPreferred,
                Some(Linkage::Static),
                true,
                ready
            ),
            Ok(Linkage::Static)
        );
        // ...and a missing archive is then an error, not a second, shared copy
        // of gfx.
        let err = resolve(
            LinkMode::DynamicPreferred,
            Some(Linkage::Static),
            true,
            unavailable,
        )
        .unwrap_err();
        assert!(err.contains("optics-gfx-sys"), "{err}");
        assert!(err.contains("OPTICS_DYNAMIC=1"), "{err}");

        let err = resolve(
            LinkMode::DynamicPreferred,
            Some(Linkage::Dynamic),
            false,
            untouched,
        )
        .unwrap_err();
        assert!(err.contains("OPTICS_STATIC=1"), "{err}");
    }

    #[test]
    fn explicit_mode_contradicting_upstream_is_rejected() {
        let err = resolve(LinkMode::Static, Some(Linkage::Dynamic), true, ready).unwrap_err();
        assert!(
            err.contains("TEST_SWITCH") && err.contains("optics-gfx-sys"),
            "{err}"
        );
        let err = resolve(LinkMode::Dynamic, Some(Linkage::Static), true, ready).unwrap_err();
        assert!(err.contains("already linked Optics static"), "{err}");
    }
}
