# ADR-0099: Desktop Platform Integration — Extended Function Keys, Focus Activation, and MIME Clipboard Interop

- Status: Accepted
- Date: 2026-09-17
- Scope: lens (L2 UI engine), iris (L3 platform toolkit)

## Context

Optics applications running on desktop environments (such as the Arca file manager, developer tools, and text utilities) require deep integration with OS platform standards. Prior to this decision, several key platform seams were either absent or lacked cross-desktop parity:

1. **Extended Function Keys (F1–F12)**: Lens defined standard navigation and editing keys (`ESCAPE`, `ENTER`, `TAB`, `BACKSPACE`, `DELETE`, `ARROWS`, `HOME`, `END`), but lacked function keys `F1` through `F12`. Essential desktop workflows—such as `F2` (rename), `F3` (search / find next), `F5` (refresh), and `F11` (toggle fullscreen)—could not be dispatched uniformly across platforms.
2. **Wayland Focus Stealing Prevention & Token Activation (`xdg-activation-v1`)**: Modern Wayland compositors (GNOME Mutter, KDE KWin, wlroots) enforce strict focus stealing prevention. When an Optics application launches or delegates to an external process (e.g. opening an editor, previewing a media file, or launching a child tool), the target window cannot reliably acquire input focus unless granted an activation token issued by the compositor. Iris lacked an API to request activation tokens.
3. **Desktop File Interoperability in Clipboard and Drag-and-Drop**: On Linux desktops, file managers (notably GNOME Files / Nautilus, Thunar, PCManFM) negotiate file copy/cut operations using the standard `x-special/gnome-copied-files` MIME payload (prefixed with `copy\n` or `cut\n`) alongside `text/uri-list`. Iris clipboard and DnD only tracked basic plain-text and generic uri-lists, leading to dropped or misparsed payloads during inter-application file transfers.

## Decision

We integrate complete desktop function keys, compositor-backed focus activation, and multi-format MIME file handling across Iris and Lens adhering to strict architectural best practices:

### 1. Extended Function Keys (`LENS_KEY_F1` .. `LENS_KEY_F12`)

- **Lens Specification (`libs/lens/include/lens/lens.h`)**:
  Define contiguous key values `LENS_KEY_F1` (280) through `LENS_KEY_F12` (291).
- **Rust Bindings (`bindings/lens-rs/crates/lens/src/input.rs`)**:
  Export `key::F1` through `key::F12` in the safe input module.
- **Cross-Platform Input Mapping**:
  Map native hardware and virtual key events to Lens keys across all three supported backends:
  - **Wayland (`libs/iris/src/app_wayland.c`)**: Map XKB keysyms `XKB_KEY_F1` through `XKB_KEY_F12`.
  - **Win32 (`libs/iris/src/app_win32.c`)**: Map Windows virtual keys `VK_F1` through `VK_F12`.
  - **macOS Cocoa (`libs/iris/src/app_cocoa.m`)**: Map macOS Carbon virtual keycodes (`kVK_F1` = 0x7A .. `kVK_F12` = 0x6F) and function key sentinels.

### 2. Window Activation Token Protocol (`xdg-activation-v1`)

- **Public Platform Contract (`libs/iris/include/iris/window.h`)**:
  Expose `iris_window_create_activation_token(const char *app_id, char *out_buf, size_t out_cap)` (with backward-compatible alias `iris_wayland_create_activation_token`).
- **Capability Discovery (`libs/iris/include/iris/capability.h`)**:
  Introduce `IRIS_CAP_ACTIVATION_TOKEN = 11`. Compile-time queries (`iris_supports(IRIS_CAP_ACTIVATION_TOKEN)`) report `1` on Wayland and `0` on Win32 and Cocoa.
- **Wayland Implementation & Event Queue Isolation (`libs/iris/src/app_wayland.c`)**:
  Bind `xdg_activation_v1` global. To prevent blocking or re-entrantly dispatching general UI/input events from the main display queue during the synchronous token handshake:
  - Create a dedicated `struct wl_event_queue *queue` and assign the `xdg_activation_token_v1` proxy to it via `wl_proxy_set_queue`.
  - Commit the token request with `app_id`, `surface`, and `last_serial`.
  - Dispatch only the dedicated queue with bounded poll timeouts (~1 second total deadline guard).
  - Destroy both the token proxy and the isolated queue upon completion.
- **Win32 & Cocoa Degradation**:
  Provide clean stubs returning `-1` without side effects.
- **Rust Bindings (`bindings/iris-rs/crates/iris/src/lib.rs`)**:
  Expose `iris::window_create_activation_token(app_id: Option<&str>) -> Option<String>` and `Capability::ActivationToken`.

### 3. Desktop Interop MIME Clipboard & Drag-and-Drop

- **Wayland Data Offer & Source Handling (`libs/iris/src/app_wayland.c`)**:
  - Support `x-special/gnome-copied-files` MIME type alongside `text/uri-list`, `text/plain;charset=utf-8`, and `UTF8_STRING`.
  - Enforce strict string length validation prior to prefix comparisons (`copy\n` requires `len >= 5`, `cut\n` requires `len >= 4`, `https://` requires `len >= 8`) to guarantee zero heap out-of-bounds reads.
  - When serving `text/uri-list` from an internal GNOME copy/cut payload, transparently strip the command header (`copy\n` / `cut\n`) so standard URI-list consumers receive valid RFC 2483 URI lines.
  - During drag-and-drop source data streaming, automatically normalize absolute UNIX paths starting with `/` into valid `file://` URIs for `text/uri-list` targets.

## Alternatives Considered

- **Alternative A: Asynchronous callback-only activation token API.**
  Rejected because application launchers (e.g. `std::process::Command::spawn`) typically run synchronously during user-initiated event handlers. Providing an isolated event queue with short polling bounds yields an intuitive, safe synchronous contract without disrupting event delivery.
- **Alternative B: Platform-prefixed API only (`iris_wayland_create_activation_token`).**
  Rejected as an architectural compromise. All functions in `<iris/window.h>` use the `iris_window_*` namespace and degrade uniformly per the capability matrix (`IRIS_CAP_ACTIVATION_TOKEN`). A backward-compatible alias is retained to avoid breaking draft bindings.
- **Alternative C: Omitting macOS Cocoa function key mapping.**
  Rejected. Incomplete platform coverage violates Optics cross-platform invariants (ADR-0056). All supported backends must offer equivalent keycode translations.

## Consequences

- Applications built on Optics can reliably implement standard shortcuts (`F1`–`F12`) across Linux, Windows, and macOS.
- Wayland applications can hand off activation tokens to launched external tools, ensuring seamless focus transitions under compositors with strict focus-stealing rules.
- Clipboard and drag-and-drop interactions between Optics applications and desktop file managers work out-of-the-box with full memory safety.
- Public symbols table (`docs/reference/symbols.md`), capability matrix, and automated tests are fully synchronized.

## References

- Wayland Protocol: `xdg-activation-v1.xml` (staging version 1)
- XDG Desktop Draft: Shared MIME-info Specification and GNOME Files clipboard convention
- ADR-0056: Win32, Cocoa backends and MoltenVK
- ADR-0086: Cross-Platform Drag-and-Drop Subsystem
- ADR-0098: Developer Tooling Taxonomy, Architecture Guardrails, and Unified Verification
