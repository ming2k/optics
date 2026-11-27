# Choose a Static or Shared Optics Link

Optics ships every module as a shared object (`liboptics_*.so`). When the Meson
tree is configured with `default_library=both` (the upstream default), it also
ships a static archive (`liboptics_*.a`). Each Rust consumer chooses which one to
link; two consumers of the same install tree do not have to agree.

This guide covers the supported configurations, how the build script decides,
and what to check when a link fails. To add Optics to a project in the first
place, see [Use Optics from a Rust Project](use-optics-from-rust.md).

---

## The configurations

The default follows the usual `-sys` crate convention: link the shared objects,
and embed the archives only when asked to.

| Selection | Result |
| :--- | :--- |
| nothing | **shared-preferred**: links `liboptics_*.so` when it exists; on a static-only tree, links the archives |
| `features = ["static"]` | **force static**: embeds the archives; fails when an archive or a private dependency is missing |
| `OPTICS_STATIC=1` | **force static**, overriding everything below it |
| `OPTICS_DYNAMIC=1` | **force shared**, overriding the `static` feature; fails when there are no shared objects |

```toml
[dependencies]
optics-app = { git = "https://github.com/ming2k/optics", features = ["static"] }
```

Each wrapper and `-sys` crate forwards `static` to its own Optics dependencies,
so enabling it on your top-level Optics dependency (for example `optics-app`,
which covers `window`, `ui`, and `gfx`) is enough. If you depend on two
unrelated native branches (such as `optics-window` and `optics-text`), enable
`static` on both or set `OPTICS_STATIC=1`.

### Why there is no `dynamic` feature

Cargo features are additive: Cargo enables a feature for the whole build as soon
as any crate in the dependency graph asks for it. A pair of mutually exclusive
`static` / `dynamic` features would therefore let two unrelated dependencies
break each other's build. Shared linking is the default, so it needs no
feature. `static` only adds a requirement.

### Environment overrides

`OPTICS_STATIC` and `OPTICS_DYNAMIC` are for whoever runs the build, such as a
packager or CI, and they take precedence over the feature in both directions.
`OPTICS_DYNAMIC=1` forces a shared link even when some dependency enabled
`static`. Setting a switch to `0`, `false`, or an empty value disables it so a
shell invocation can cancel a worktree `.cargo/config.toml` `[env]` entry (for
example `OPTICS_DYNAMIC=0 cargo build` to test the default `static` feature).

`PKG_CONFIG_ALL_STATIC` / `PKG_CONFIG_ALL_DYNAMIC` are accepted as aliases, so a
`pkg-config`-wide setting cannot silently disagree with the emitted directives.
Two variables of the same direction are harmless. One of each direction,
including a mixed pair such as `OPTICS_STATIC` + `PKG_CONFIG_ALL_DYNAMIC`, is a
build error naming both, because the caller has asked for two incompatible
artifacts.

The mode is always explicit: `optics-build` passes `statik(false)` to the initial
probe, so the emitted link directives never depend on an environment variable the
helper did not read.

### One mode per build

Every Optics `-sys` crate in one build links the same way. Without coordination,
a mixed install could embed `liboptics_gfx.a` while `liboptics_ui.so`, which
itself needs `liboptics_gfx.so`, loaded a second copy of gfx with its own
globals.

To prevent this, each `-sys` crate publishes the linkage it chose
(`cargo:link_mode=static|dynamic`, read back as `DEP_OPTICS_<MOD>_LINK_MODE`).
Every module depends on gfx, so gfx makes the decision. A dependent in the
default mode adopts it and fails rather than diverging. An explicit request that
contradicts it is rejected with both crates named.

---

## Why static, and why not always

Static linking is the right choice when the artifact should not carry a runtime
dependency on a pre-1.0 ABI that changes in patch releases: a released
application binary, a single-file test harness, a container image. The Optics
SONAMEs carry the full version, so a mismatched shared install fails at load
time. Embedding the archive removes that failure mode instead of relocating it.
A static link also needs no runtime library path; see
[Make the shared libraries loadable](use-optics-from-rust.md#5-make-the-shared-libraries-loadable).

Shared linking is the right choice when several processes should share one
Optics copy, when the embedder ships updates independently of Optics, or while
iterating on the C sources, where relinking after every change is wasted work.

Static linking is scoped to the Optics modules. System libraries stay dynamic: a
fully static process image does not link on this stack (`liboptics_gfx.a`
references `__tls_get_addr`, and the Vulkan loader has to `dlopen` ICD drivers).

---

## What the static branch does

When the mode resolves to static, `optics-build`:

1. locates `liboptics_<mod>.a` in the pkg-config `libdir`, for the module and
   for every other Optics module in its static closure;
2. re-probes with `pkg-config --static` so `Requires.private` (Vulkan,
   `wayland-client`, `xkbcommon`, `fontconfig`, `freetype2`, `harfbuzz`,
   `fribidi`, `libsystemd`) is emitted. The default probe drops it, and the link
   then fails with undefined symbols;
3. emits `cargo:rustc-link-lib=static:-bundle=<mod>` for every Optics module and
   a plain `cargo:rustc-link-lib=<system lib>` for everything else;
4. publishes an empty `rpaths` list, because nothing has to be found at runtime.

The linker therefore sees `-Wl,-Bstatic -loptics_app ... -Wl,-Bdynamic -lvulkan
...`: only the Optics span is static.

The shared branch never runs the `--static` probe. It publishes the non-system
library directories so the crates in this workspace can relay them into
`-Wl,-rpath`.

On Windows, Meson names static archives `liboptics_<mod>.a`, because
`optics_<mod>.lib` is already the DLL's import library. `optics-build` therefore
looks for `liboptics_<mod>.a` only and links it by exact file name
(`static:-bundle,+verbatim=liboptics_<mod>.a`). For a shared link it checks for
the import library (`optics_<mod>.lib` or `liboptics_<mod>.dll.a`), not for the
DLL, which an installed prefix keeps in `bin/`.

---

## Preparing the Optics build

Either library flavour requires the uninstalled Meson tree or an installed prefix
on `PKG_CONFIG_PATH`.

```bash
# Both flavours (the default; every link mode works):
meson setup build -Dtests=false
meson compile -C build

# Static only: no shared objects at all; the default mode links the archives.
meson setup build -Dtests=false -Ddefault_library=static
meson compile -C build

# Shared only: the `static` feature and OPTICS_STATIC fail.
meson setup build -Dtests=false -Ddefault_library=shared
meson compile -C build
```

Verify what the host provides:

```bash
pkg-config --modversion optics-app
pkg-config --variable=libdir optics-app
ls "$(pkg-config --variable=libdir optics-app)"/liboptics_*
pkg-config --static --libs optics-ui    # must list the private system deps
```

---

## Troubleshooting

| Symptom | Cause | Fix |
| :--- | :--- | :--- |
| ``Optics was asked to link ... statically (the `static` Cargo feature), but no liboptics_*.a was found`` | The tree or prefix ships only shared objects | Rebuild with `-Ddefault_library=both`, remove the feature, or set `OPTICS_DYNAMIC=1` |
| `Optics was asked to link ... dynamically (OPTICS_DYNAMIC), but no shared library was found` | The tree was configured `-Ddefault_library=static`, so `-l<stem>` would silently resolve to the archive | Rebuild with `=both` (or `=shared`), or unset `OPTICS_DYNAMIC` |
| `OPTICS_STATIC and OPTICS_DYNAMIC request opposite Optics link modes` | Both switches are set (often one inherited from the shell) | Unset one of them |
| `... statically (to match optics-gfx-sys, which linked Optics static), but no liboptics_<mod>.a was found` | Mixed tree: gfx is static-only, a dependent module is shared-only | Rebuild with `-Ddefault_library=both`, or set `OPTICS_DYNAMIC=1` |
| `... asks for a static link of ..., but optics-gfx-sys already linked Optics dynamic` | `static` was enabled on one branch or `-sys` crate but not on gfx | Enable `static` on every top-level `optics-*` dependency (or `optics-app`), or set `OPTICS_STATIC=1` |
| `undefined symbol: FcPatternBuild` / `wl_touch_interface` / `sd_bus_call_method_async` | `Requires.private` was dropped: the static link is missing the module's private dependencies | Confirm the dev packages are installed and that `pkg-config --static --libs <module>` lists them |
| `linking ... statically needs ... the pkg-config --static probe failed` | A private dependency's `.pc` file is missing | Install that dependency's development package |
| `liboptics_*.so.0.0.NN: cannot open shared object file` when the binary starts | The shared flavour was linked, and the loader cannot find it or finds a different patch release | See [Make the shared libraries loadable](use-optics-from-rust.md#5-make-the-shared-libraries-loadable), or enable `static` |

---

## See also

- [Use Optics from a Rust Project](use-optics-from-rust.md): adding the
  dependency, pointing the build at the C libraries, and running the result.
- [Rust Bindings](../explanation/rust-bindings.md): how the `-sys` crates and
  `optics-build` discover headers and libraries.
