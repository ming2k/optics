//! Safe Rust bindings to **ui** — an immediate-mode UI façade over a
//! retained-mode core, drawing through gfx's Vulkan canvas.
//!
//! Three ways in, lowest to highest level:
//!
//! 1. **Headless** ([`Ui::headless`]) — logic/layout/interaction with no GPU.
//! 2. **Embedded** ([`Ui::with_device`] + [`Snapshot::submit`]) — you own a gfx
//!    device and canvas; ui draws the UI into your frame. This is the
//!    seam for putting UI inside an existing gfx/Vulkan app.
//! 3. **Windowed** — use the `optics-app` crate, which owns the window,
//!    device, and event loop and hands you a [`Frame`] each frame.
//!
//! ```no_run
//! let mut ui = optics_ui::Ui::headless().unwrap();
//! let input = optics_ui::Input::default();
//! ui.frame(&input, |f| {
//!     f.column().show(|f| {
//!         f.label("Settings");
//!         let mut wrap = true;
//!         f.checkbox("Wrap", &mut wrap);
//!         if f.button("Save") { /* ... */ }
//!     });
//! });
//! ```

use std::ffi::CString;
use std::os::raw::c_char;
use std::ptr;

/// The 2D vector canvas and Vulkan rendering foundation (`CornerRadii`, etc.).
pub use optics_gfx as gfx;

/// Raw-bindings escape hatch (see the `gfx` crate's note). Kept `pub`
/// deliberately as the one documented unsafe escape hatch.
pub use optics_ui_sys as sys;

/// The caret blink period ui applies when a context does not override it
/// (Optics ADR-0151): one full on/off cycle, so a 530 ms half-cycle — the
/// cadence X11, Win32, AppKit and GTK converged on.
///
/// Mirrors `OPTICS_UI_CARET_BLINK_PERIOD_S` in ui's internal header. It is exposed
/// because "restore the default blink" has no entry point: [`Ui::set_caret_blink`]
/// spells "no blink" as `period <= 0`, so a host that disabled the blink for an
/// accessibility preference must name a period to bring it back. Without this
/// constant every such host would hardcode the number and silently drift from
/// the library.
pub const DEFAULT_CARET_BLINK_PERIOD_S: f32 = 1.06;

/// Owned immutable visual publication. It may outlive its source UI.
pub struct Snapshot {
    raw: *mut sys::optics_ui_scene_snapshot,
}

impl Snapshot {
    pub fn generation(&self) -> u64 {
        // SAFETY: self owns a live snapshot.
        unsafe { sys::optics_ui_snapshot_generation(self.raw) }
    }

    /// Take a second owner of this snapshot.
    ///
    /// `optics_ui_snapshot_retain` refcounts, but the Rust handle models a single
    /// owner, so a caller that needs to keep one scene *and* hand it to
    /// `activate`/`submit` — a host caching a last-good scene, for instance —
    /// had no way to express it. Without this the only options were to copy the
    /// display list or to give up the reference, and giving it up is exactly
    /// what a fallback scene must not do.
    ///
    /// Both handles must be dropped independently; each releases one reference.
    #[must_use]
    pub fn retain(&self) -> Snapshot {
        // SAFETY: self owns a live snapshot; retain is atomic and returns the
        // same pointer, which the new owner releases in its own Drop.
        let raw = unsafe { sys::optics_ui_snapshot_retain(self.raw) };
        Snapshot { raw }
    }

    /// Execute this snapshot inside an open Canvas session.
    ///
    /// # Safety
    /// `canvas` must be live, exclusively writable, and inside an open session
    /// on the device that owns any GPU resources captured by this snapshot.
    pub unsafe fn submit(&self, canvas: *mut sys::optics_gfx_canvas) -> Result<(), Error> {
        // SAFETY: caller guarantees the canvas state; self retains the snapshot.
        let rc = unsafe { sys::optics_ui_snapshot_submit(self.raw, canvas) };
        if rc != sys::optics_gfx_result::OPTICS_GFX_OK {
            return Err(Error::from_result(Error::Render, rc));
        }
        Ok(())
    }
}

impl Drop for Snapshot {
    fn drop(&mut self) {
        // SAFETY: release exactly this owner's reference.
        unsafe { sys::optics_ui_snapshot_release(self.raw) };
    }
}

// SAFETY: Snapshot is an owned, immutable visual publication; internal refcounts
// are atomic and reading its geometry/generation is thread-safe.
unsafe impl Send for Snapshot {}
unsafe impl Sync for Snapshot {}

mod input;
pub mod motion;
pub mod patterns;
pub mod reactive;
mod types;
pub mod view;

pub use input::{Input, MouseButton, key, mods};
pub use optics_gfx::CornerRadii;
pub use patterns::{SegmentedControl, SegmentedItem};
pub use types::{
    Align, Band, ButtonVariant, CheckboxAppearance, Color, CursorHint, DndDropInfo, FontFamily,
    Icon, LabelOpts, LayoutOpts, PlaceMode, PlaceOpts, Rect, Response, SkinFn, SliderOpts, Style,
    StyleResolved, TextEditOpts, TextLine, TextMetrics, Theme, WidgetContent, WidgetKind,
    WidgetRecord, WidgetState,
};

/// The retained UI context. Owns the persistent tree, layout, and draw list.
/// Dropping a `Ui` calls `optics_ui_release`.
pub struct Ui {
    raw: *mut sys::optics_ui,
    /// True when this handle was borrowed (`borrow_raw`) rather than created;
    /// borrowed handles do NOT destroy the context on drop.
    borrowed: bool,
}

impl Ui {
    /// Create a headless context (no gfx device): immediate-mode logic,
    /// layout and snapshot publication run without a GPU. Activate a snapshot
    /// explicitly to use its geometry for headless interaction.
    pub fn headless() -> Result<Ui, Error> {
        Self::create(ptr::null_mut())
    }

    /// Wrap a live `ui*` you do not own as a borrowed (non-owning) view.
    /// The returned `Ui` does **not** destroy the context on drop — the
    /// owner (e.g. window, for its app context) stays in charge. Use this
    /// when a host hands you a live ui context (inside a start/setup
    /// callback) and you want the safe surface without taking ownership.
    ///
    /// # Safety
    /// `raw` must be a live `ui*` that remains valid for as long as the
    /// returned `Ui` (or any `&Ui` derived from it) is used, and must not
    /// be used through any other mutable view simultaneously.
    pub unsafe fn borrow_raw(raw: *mut sys::optics_ui) -> Ui {
        Ui {
            raw,
            borrowed: true,
        }
    }

    /// Create a context bound to an existing gfx device. The device is
    /// retained for the lifetime of the `Ui`. Use this to embed ui into
    /// an app that already manages its own gfx/Vulkan setup; pair with
    /// [`Snapshot::submit`] inside your canvas envelope.
    ///
    /// # Safety
    /// `device` must be a live `optics_gfx_device` obtained from the gfx API and
    /// must remain valid until this `Ui` is dropped.
    pub unsafe fn with_device(device: *mut sys::optics_gfx_device) -> Result<Ui, Error> {
        Self::create(device)
    }

    fn create(device: *mut sys::optics_gfx_device) -> Result<Ui, Error> {
        // SAFETY: a zeroed desc with the given device is valid; theme is set to
        // the dark token set, all other fields fall back to library defaults.
        let desc = sys::optics_ui_desc {
            device,
            theme: unsafe { sys::optics_ui_theme_dark() },
            ..unsafe { std::mem::zeroed() }
        };
        let mut out: *mut sys::optics_ui = ptr::null_mut();
        // SAFETY: desc is fully initialized; out is a valid slot.
        let rc = unsafe { sys::optics_ui_create(&desc, &mut out) };
        if rc != sys::optics_gfx_result::OPTICS_GFX_OK || out.is_null() {
            return Err(Error::from_result(Error::Create, rc));
        }
        Ok(Ui {
            raw: out,
            borrowed: false,
        })
    }

    /// Whether the retained store overflowed: an id ring wrapped or the
    /// node pool filled, so some widgets from earlier frames may have lost
    /// their retained state. Long-lived hosts can surface this in
    /// diagnostics; ordinary UIs never hit it.
    pub fn overflowed(&self) -> bool {
        // SAFETY: self.raw is live; the call only reads state.
        unsafe { sys::optics_ui_overflowed(self.raw as *const sys::optics_ui) }
    }

    /// Whether one parent received the same retained widget id twice in the
    /// last frame. Repeated data rows should use an explicit widget id or an
    /// id scope; otherwise ui can only retain one of the duplicate nodes.
    pub fn has_duplicate_ids(&self) -> bool {
        // SAFETY: self.raw is live; the call only reads frame diagnostics.
        unsafe { sys::optics_ui_has_duplicate_ids(self.raw as *const sys::optics_ui) }
    }

    /// Deliver a drag-and-drop payload into the UI at the given position.
    pub fn deliver_drop(&mut self, payload: &str, pos: (f32, f32)) {
        let c = cstr(payload);
        let pt = sys::optics_gfx_point { x: pos.0, y: pos.1 };
        let opts = sys::optics_ui_deliver_drop_opts {
            payload: c.as_ptr(),
            len: payload.len(),
            pos: pt,
        };
        unsafe { sys::optics_ui_deliver_drop(self.raw, &opts) };
    }

    /// Register a format-agnostic vector icon on this UI context.
    ///
    /// The command stream lives in a 24x24 logical box and is deep-copied into
    /// context-owned storage. Returns `None` if `cmds` is empty or invalid.
    pub fn register_icon(
        &mut self,
        cmds: &[sys::optics_ui_icon_cmd],
        runs: &[sys::optics_ui_icon_run],
        paint: sys::optics_ui_icon_paint,
    ) -> Option<sys::optics_ui_icon_id> {
        let desc = sys::optics_ui_icon_desc {
            cmds: cmds.as_ptr(),
            count: u32::try_from(cmds.len()).ok()?,
            runs: if runs.is_empty() {
                ptr::null()
            } else {
                runs.as_ptr()
            },
            run_count: u32::try_from(runs.len()).ok()?,
            paint,
            _pad: [0; 3],
        };
        // SAFETY: self.raw is live; cmds and runs outlive the call and are deep-copied by C.
        let id = unsafe { sys::optics_ui_icon_register(self.raw, &desc) };
        if id == sys::optics_ui_icon_id::OPTICS_UI_ICON_NONE {
            None
        } else {
            Some(id)
        }
    }

    /// Unregister a previously registered runtime vector icon on this UI context.
    pub fn unregister_icon(&mut self, id: sys::optics_ui_icon_id) {
        // SAFETY: self.raw is live; C ignores unknown or built-in ids.
        unsafe { sys::optics_ui_icon_unregister(self.raw, id) };
    }

    /// Run one immediate-mode frame. The closure receives a [`Frame`] that
    /// borrows the context; widget calls on it build the tree, which is
    /// reconciled and laid out when the closure returns.
    pub fn frame<R>(
        &mut self,
        input: &Input,
        build: impl for<'frame> FnOnce(&mut Frame<'frame>) -> R,
    ) -> R {
        // SAFETY: self.raw is live; input outlives the call.
        unsafe { sys::optics_ui_begin(self.raw, input.as_raw()) };
        struct EndFrame(*mut sys::optics_ui);
        impl Drop for EndFrame {
            fn drop(&mut self) {
                // SAFETY: guard is confined to the owning Ui borrow.
                unsafe { sys::optics_ui_end(self.0) };
            }
        }
        let _end = EndFrame(self.raw);
        // SAFETY: raw is live and inside a begin/end pair.
        let mut f = unsafe { Frame::from_raw(self.raw) };
        build(&mut f)
    }

    /// Publish owned visuals for the last built frame.
    pub fn snapshot(&mut self) -> Result<Snapshot, Error> {
        let mut raw = ptr::null_mut();
        // SAFETY: the UI is uniquely borrowed and the output slot is valid.
        let rc = unsafe { sys::optics_ui_snapshot_create(self.raw, &mut raw) };
        if rc != sys::optics_gfx_result::OPTICS_GFX_OK {
            return Err(Error::from_result(Error::Render, rc));
        }
        Ok(Snapshot { raw })
    }

    /// Activate the snapshot that the host successfully presented.
    pub fn activate(&mut self, snapshot: &Snapshot) -> Result<(), Error> {
        // SAFETY: both objects are live; C validates their identity and generation.
        let rc = unsafe { sys::optics_ui_snapshot_activate(self.raw, snapshot.raw) };
        if rc != sys::optics_gfx_result::OPTICS_GFX_OK {
            return Err(Error::from_result(Error::Render, rc));
        }
        Ok(())
    }

    /// The raw context pointer, for reaching past the safe surface.
    pub fn as_raw(&mut self) -> *mut sys::optics_ui {
        self.raw
    }

    /// Set the device-pixel (HiDPI) scale. Layout, input, and
    /// [`Input::set_display_size`] stay in logical pixels; [`Snapshot::submit`]
    /// scales the canvas transform so 1 logical pixel maps to `scale` device
    /// pixels and rasterises text crisply. Report the window-system scale here.
    pub fn set_scale(&mut self, scale: f32) {
        // SAFETY: raw is live.
        unsafe { sys::optics_ui_set_scale(self.raw, scale) };
    }

    /// Accessibility text scale — the OS "make text bigger" preference. A
    /// pure multiplier on every font-size token: glyphs, caret metrics, and
    /// every widget height derived from a font token grow together, so text
    /// scales without clipping. Orthogonal to [`Ui::set_scale`] (device
    /// pixels): a 1.25 text scale at 2x DPI renders 1.25x taller glyphs at
    /// the same 2x raster density. Pure-px geometry (padding, stroke widths)
    /// stays put. Non-finite and non-positive values are ignored. Default
    /// 1.0.
    pub fn set_text_scale(&mut self, factor: f32) {
        // SAFETY: raw is live.
        unsafe { sys::optics_ui_set_text_scale(self.raw, factor) };
    }

    /// The current accessibility text-scale factor (default 1.0).
    pub fn text_scale(&self) -> f32 {
        // SAFETY: raw is a live context; the call only reads state.
        unsafe { sys::optics_ui_text_scale(self.raw as *const sys::optics_ui) }
    }

    /// Replace the active theme token set (e.g. light/dark) live.
    pub fn set_theme(&mut self, theme: Theme) {
        // SAFETY: raw is live; theme is a value type.
        unsafe { sys::optics_ui_set_theme(self.raw, theme.0) };
    }

    // ---- transient placement (host-side open/close, ADR-0060) --------------
    //
    // `Frame::place_open` / `Frame::place_close` / `Frame::place` cover the
    // common case of driving a transient popup from inside a frame. These
    // mirror the same calls on `Ui` so a host can open or dismiss a popup
    // outside a frame — e.g. from a keyboard handler that runs between
    // frames. The state is retained per id on the context, so a `close` here
    // is visible to the next frame's `Frame::place` / `Frame::place_is_open`.

    /// Open the transient place node keyed by `id` (retained until closed).
    /// Safe to call outside a frame; the body renders on the next
    /// [`Ui::frame`].
    pub fn place_open(&mut self, id: &str) {
        let c = cstr(id);
        // SAFETY: raw is live; c outlives the call.
        unsafe { sys::optics_ui_place_open(self.raw, c.as_ptr()) };
    }

    /// Close the transient place node keyed by `id`. Safe to call outside a
    /// frame.
    pub fn place_close(&mut self, id: &str) {
        let c = cstr(id);
        // SAFETY: raw is live; c outlives the call.
        unsafe { sys::optics_ui_place_close(self.raw, c.as_ptr()) };
    }

    /// Whether the transient place node keyed by `id` is currently open.
    /// Safe to call outside a frame.
    pub fn place_is_open(&mut self, id: &str) -> bool {
        let c = cstr(id);
        // SAFETY: raw is live; c outlives the call; read-only.
        unsafe { sys::optics_ui_place_is_open(self.raw as *const sys::optics_ui, c.as_ptr()) }
    }

    /// Whether an eased value (hover/active fade, …) was still in transit
    /// during the frame just built. An input-driven host that paints only on
    /// events should request one more frame while this holds, so animations
    /// settle to rest instead of freezing mid-fade when input stops. Valid
    /// after [`Ui::frame`] returns, until the next frame begins.
    pub fn anim_pending(&self) -> bool {
        // SAFETY: raw is a live context; the call only reads state.
        unsafe { sys::optics_ui_anim_pending(self.raw as *const sys::optics_ui) }
    }

    /// The damaged area of the frame just built as rectangles in UI-space,
    /// written into `out`.
    ///
    /// Returns the number of rectangles written. Every rectangle is covered and
    /// none is contained in another, so a host may scissor each one, hand the
    /// whole list to a per-plane damage hint, or — when its presentation API
    /// takes a single rectangle — union them itself with `Rect::union` at that
    /// boundary. There is deliberately no single-rect accessor: collapsing inside
    /// the producer would merge a status-chip tick and a dock spring into one
    /// nearly-full-output box before any consumer could use the set.
    ///
    /// When the damage needs more rectangles than `out` holds, the answer is a
    /// single covering rectangle in `out[0]` and the return value is `1`: a
    /// truncated list would leave stale pixels on screen, while over-covering
    /// only costs a hint. Pass an empty slice to ask for the count alone, which
    /// is how a caller sizes its buffer without guessing.
    pub fn damage_rects(&self, out: &mut [Rect]) -> usize {
        // The C contract's bound. A stack buffer keeps the query allocation-free;
        // the slice length decides how much of it the caller actually wants.
        const CAPACITY: usize = 64;
        let mut raw = [sys::optics_gfx_rect {
            x: 0.0,
            y: 0.0,
            w: 0.0,
            h: 0.0,
        }; CAPACITY];
        let capacity = out.len().min(CAPACITY);
        let count = unsafe {
            sys::optics_ui_damage_rects(
                self.raw as *const sys::optics_ui,
                &sys::optics_ui_damage_opts {
                    rects: if capacity == 0 {
                        std::ptr::null_mut()
                    } else {
                        raw.as_mut_ptr()
                    },
                    capacity: capacity as u32,
                },
            )
        } as usize;
        // The count is the true number the damage resolves to, which can exceed
        // what was written; only initialized slots may be copied out.
        let written = count.min(capacity);
        for (slot, source) in out.iter_mut().zip(raw.iter()).take(written) {
            *slot = Rect::from_raw(*source);
        }
        written
    }

    /// The semantic cursor shape requested by the topmost hovered control on
    /// the most recently built frame (ADR-0044). Defaults to [`CursorHint::Default`].
    pub fn cursor_hint(&self) -> CursorHint {
        // SAFETY: raw is a live context; the call only reads state.
        CursorHint::from_raw(unsafe {
            sys::optics_ui_get_cursor_hint(self.raw as *const sys::optics_ui)
        })
    }

    // ---- keyboard focus (host side) ---------------------------------------
    //
    // Focus is host state, not a draw operation: a host transfers it from an
    // input handler or a keybinding, which runs between frames. These live on
    // `Ui` for that reason, and they take a label rather than an id because an
    // id is only meaningful against the id stack the last build populated — a
    // caller that resolved it separately could hold a stale one.

    /// Give keyboard focus to the widget built under `label`, reporting whether
    /// it took.
    ///
    /// `false` when no widget carries that label — it is not registered, or
    /// nothing has been built under it yet.
    pub fn focus(&mut self, label: &str) -> bool {
        let c = cstr(label);
        // SAFETY: raw is live; `c` outlives the call.
        let id = unsafe { sys::optics_ui_current_id(self.raw, c.as_ptr()) };
        if id == 0 {
            return false;
        }
        // SAFETY: raw is live; `id` came from this context's own id stack.
        unsafe { sys::optics_ui_set_focus(self.raw, id) };
        true
    }

    /// Whether the widget built under `label` currently holds keyboard focus.
    ///
    /// `false` for an unresolvable label, which is also the honest answer for
    /// "nothing here is focused".
    pub fn is_focused(&self, label: &str) -> bool {
        let c = cstr(label);
        // SAFETY: raw is a live context; the call only reads state.
        let id = unsafe { sys::optics_ui_current_id(self.raw, c.as_ptr()) };
        if id == 0 {
            return false;
        }
        // SAFETY: raw is a live context; the call only reads state.
        unsafe { sys::optics_ui_focused(self.raw as *const sys::optics_ui, id) }
    }

    /// Release keyboard focus from whatever holds it.
    pub fn clear_focus(&mut self) {
        // SAFETY: raw is live; 0 is the documented "no widget" id.
        unsafe { sys::optics_ui_set_focus(self.raw, 0) };
    }

    /// Accessibility reduced-motion switch. While enabled, every eased value
    /// resolves to its target within one frame — no fades or slides — and
    /// [`Ui::anim_pending`] stays false. The host owns the policy (user
    /// preference); ui executes it. Default false.
    pub fn set_reduced_motion(&mut self, reduced: bool) {
        // SAFETY: raw is a live context.
        unsafe { sys::optics_ui_set_reduced_motion(self.raw, reduced) };
    }

    /// Whether reduced motion is currently enabled.
    pub fn reduced_motion(&self) -> bool {
        // SAFETY: raw is a live context; the call only reads state.
        unsafe { sys::optics_ui_reduced_motion(self.raw as *const sys::optics_ui) }
    }

    // ---- clipboard & IME (host side, ADR-0013) ----------------------------

    /// Caret rect of the focused text widget, in UI-space; zero-sized when no
    /// text widget is focused. The host forwards this to the platform IME
    /// (e.g. `zwp_text_input`) so the candidate window can position itself.
    ///
    /// This is *geometry* only: it says where the caret is, never whether it is
    /// painted this frame. [`Ui::caret_visible`] answers that.
    pub fn caret_rect(&self) -> Rect {
        // SAFETY: raw is a live context; the call only reads state.
        Rect::from_raw(unsafe { sys::optics_ui_caret_rect(self.raw) })
    }

    /// Current caret blink phase: `true` while the caret is in the visible half
    /// of its blink cycle (Optics ADR-0151).
    ///
    /// ui owns the blink clock because only ui can make it a *damage*
    /// signal. The clock advances once per build by `Input::dt_seconds`, so a
    /// host paced from the display pulse gets a display-sampled blink for free,
    /// and a host that skipped frames resumes on the phase it would have
    /// reached. A skin checks this before drawing the caret; the host never has
    /// to model a blink at all.
    ///
    /// Always `true` while an IME composition is in progress — frames are
    /// event-driven then, and a blink would freeze mid-composition.
    pub fn caret_visible(&self) -> bool {
        // SAFETY: raw is a live context; the call only reads state.
        unsafe { sys::optics_ui_caret_visible(self.raw as *const sys::optics_ui) }
    }

    /// Override the caret blink period in seconds (Optics ADR-0151).
    ///
    /// `period <= 0` (or a non-finite value) *disables* the blink: the caret
    /// stays solid and ui reports no caret damage at all, which is how
    /// reduced motion is expressed. Use [`DEFAULT_CARET_BLINK_PERIOD_S`] to
    /// restore the library default, since there is no separate "reset" call.
    ///
    /// Accessibility preference is host policy, so the host decides; ui
    /// executes.
    pub fn set_caret_blink(&mut self, period_seconds: f32) {
        // SAFETY: raw is live; the value is a plain float.
        unsafe { sys::optics_ui_set_caret_blink(self.raw, period_seconds) };
    }

    /// `true` when the caret's blink phase differs from the phase the last
    /// activated snapshot put on screen (Optics ADR-0151).
    ///
    /// This is the host's whole caret-damage contract: forward it to the frame
    /// pacer as "one frame is owed", and never model a blink yourself.
    /// Comparing against the *presented* phase rather than the last built one is
    /// what makes a skipped frame keep its flip pending instead of dropping it,
    /// and it is why a host cannot derive this from [`Ui::caret_visible`] alone
    /// (that is only the current phase, with no memory of what reached the
    /// display).
    ///
    /// `false` when no caret is focused: a phase flip nobody can see is not
    /// damage. Read between the frame build and the next one.
    pub fn caret_flip_pending(&self) -> bool {
        // SAFETY: raw is a live context; the call only reads state.
        unsafe { sys::optics_ui_caret_flip_pending(self.raw as *const sys::optics_ui) }
    }

    /// `true` when a caret blink clock is running: a text widget holds focus and
    /// the blink is enabled (Optics ADR-0151).
    ///
    /// Distinct from [`Ui::caret_flip_pending`], and the two are not
    /// interchangeable. The clock only advances inside a build (it integrates
    /// `Input::dt_seconds`), so a host that stopped producing frames could never
    /// observe the next flip — the caret would freeze mid-blink. This level
    /// therefore tells the pacer to keep its *low idle tick* alive; it does not
    /// ask for a paint, and it must not buy the active-rate cadence. The paint
    /// is owed separately, by the frame on which the flip lands.
    pub fn caret_clock_live(&self) -> bool {
        // SAFETY: raw is a live context; the call only reads state.
        unsafe { sys::optics_ui_caret_clock_live(self.raw as *const sys::optics_ui) }
    }

    /// Place text on the system clipboard (via the host clipboard interface, if
    /// one was supplied to the context). No-op otherwise.
    pub fn copy(&mut self, text: &str) {
        let opts = sys::optics_ui_copy_opts {
            utf8: text.as_ptr() as *const c_char,
            len: text.len(),
        };
        // SAFETY: raw is live; text/len describe a valid byte range.
        unsafe { sys::optics_ui_copy(self.raw, &opts) };
    }

    /// Ask the host for clipboard text; it is later delivered via [`Ui::paste`].
    pub fn request_paste(&mut self) {
        // SAFETY: raw is live.
        unsafe { sys::optics_ui_request_paste(self.raw) };
    }

    /// Deliver clipboard text into ui; the focused text widget consumes it
    /// on the next frame's build.
    pub fn paste(&mut self, text: &str) {
        let opts = sys::optics_ui_paste_opts {
            utf8: text.as_ptr() as *const c_char,
            len: text.len(),
        };
        // SAFETY: raw is live; text/len describe a valid byte range.
        unsafe { sys::optics_ui_paste(self.raw, &opts) };
    }
}

impl Drop for Ui {
    fn drop(&mut self) {
        if self.borrowed {
            return; // the owner (e.g. window) destroys the context
        }
        // SAFETY: raw was created by optics_ui_create and not yet destroyed.
        unsafe { sys::optics_ui_release(self.raw) };
    }
}

/// Builder handle valid only for the duration of one frame. It borrows the
/// context and must not escape the frame closure. Independent frame scopes
/// cannot exchange their handles:
/// ```compile_fail
/// use optics_ui::{Ui, Input};
/// let mut a = Ui::headless().unwrap();
/// let mut b = Ui::headless().unwrap();
/// a.frame(&Input::default(), |fa| {
///     b.frame(&Input::default(), |fb| std::mem::swap(fa, fb));
/// });
/// ```
pub struct Frame<'frame> {
    ui: *mut sys::optics_ui,
    _scope: std::marker::PhantomData<&'frame mut &'frame ()>,
}

impl<'frame> Frame<'frame> {
    /// Wrap a raw `ui` that the caller has already `optics_ui_begin`'d. Used by
    /// `ui-shell-wayland`, whose host owns the begin/end envelope.
    ///
    /// # Safety
    /// `ui` must be a live context currently inside a `optics_ui_begin` /
    /// `optics_ui_end` pair, and the returned `Frame` must not outlive it.
    pub unsafe fn from_raw(ui: *mut sys::optics_ui) -> Frame<'frame> {
        Frame {
            ui,
            _scope: std::marker::PhantomData,
        }
    }

    /// The raw context pointer, for widgets not yet wrapped here.
    pub fn as_raw(&mut self) -> *mut sys::optics_ui {
        self.ui
    }

    /// The active theme token set for this frame. Use it to pick colours that
    /// stay correct when the app runs in light or dark mode.
    pub fn theme(&self) -> Theme {
        // SAFETY: ui is live inside a frame; optics_ui_get_theme only reads state.
        Theme(unsafe { sys::optics_ui_get_theme(self.ui) })
    }

    /// Replace the active theme token set for subsequent widgets in this frame.
    pub fn set_theme(&mut self, theme: Theme) {
        // SAFETY: ui is live inside a frame; theme is a value type.
        unsafe { sys::optics_ui_set_theme(self.ui, theme.0) };
    }

    /// Frame-scoped opacity switch (0..1, clamped; default 1.0): the single
    /// fade knob for enter/exit motion. Every node built while an opacity is
    /// in effect carries it as a build-time stamp, and emission bakes it into
    /// each draw command's colour alpha — rects, borders, text, icons, host
    /// images and scrollbars fade together, with no per-colour work by the
    /// caller. The switch resets to 1.0 at every frame begin, so a forgotten
    /// restore cannot dim the next frame; within a frame, set it back after
    /// building the faded subtree.
    pub fn set_opacity(&mut self, opacity: f32) {
        // SAFETY: ui is live inside a frame.
        unsafe { sys::optics_ui_set_opacity(self.ui, opacity) };
    }

    /// The opacity currently applied to built widgets.
    pub fn opacity(&self) -> f32 {
        // SAFETY: ui is live inside a frame; the call only reads state.
        unsafe { sys::optics_ui_opacity(self.ui as *const sys::optics_ui) }
    }

    /// Intersect every node built until [`Frame::clip_end`] with `rect`.
    ///
    /// This is the scoped clip for chrome overlays whose children are placed
    /// absolutely (and therefore escape their parent's flow viewport). The
    /// clip is a build-time stamp: it follows a placed node into its band and
    /// clips its paint, snapshot/hit region, and descendants. Nested calls
    /// intersect; the stack resets at every frame. Always pair with
    /// [`Frame::clip_end`].
    pub fn clip_begin(&mut self, rect: Rect) {
        let opts = sys::optics_ui_clip_opts {
            rect: rect.to_raw(),
        };
        // SAFETY: ui is live inside a frame; opts is a value type.
        unsafe { sys::optics_ui_clip_begin(self.ui, &opts) };
    }

    /// Pop the innermost [`Frame::clip_begin`] scope.
    pub fn clip_end(&mut self) {
        // SAFETY: ui is live inside a frame.
        unsafe { sys::optics_ui_clip_end(self.ui) };
    }

    /// Set the typeface family for subsequently built widgets in this frame.
    /// Set it, build the widgets that need another voice, then set it back —
    /// the same pattern as [`Frame::set_theme`].
    pub fn set_text_family(&mut self, family: FontFamily) {
        // SAFETY: ui is live inside a frame; family is a value type.
        unsafe { sys::optics_ui_set_text_family(self.ui, family.raw()) };
    }

    /// The typeface family currently applied to built widgets.
    pub fn text_family(&self) -> FontFamily {
        // SAFETY: ui is live inside a frame; the call only reads state.
        match unsafe { sys::optics_ui_get_text_family(self.ui as *const sys::optics_ui) } {
            sys::optics_ui_text_family::OPTICS_UI_TEXT_FAMILY_SANS => FontFamily::Sans,
            sys::optics_ui_text_family::OPTICS_UI_TEXT_FAMILY_SERIF => FontFamily::Serif,
            sys::optics_ui_text_family::OPTICS_UI_TEXT_FAMILY_MONO => FontFamily::Mono,
            _ => FontFamily::Sans,
        }
    }

    /// Push an ID seed string onto the UI's ID generator stack.
    pub fn push_id(&mut self, seed: &str) {
        let c = cstr(seed);
        // SAFETY: ui is live for the frame.
        unsafe { sys::optics_ui_push_id(self.ui, c.as_ptr()) };
    }

    /// Push an allocation-free integer scope for repeated data rows.
    pub fn push_id_int(&mut self, seed: i64) {
        // SAFETY: ui is live and the integer is copied by value.
        unsafe { sys::optics_ui_push_id_int(self.ui, seed) };
    }

    /// Pop the most recent [`Self::push_id`].
    pub fn pop_id(&mut self) {
        // SAFETY: ui is live for the frame.
        unsafe { sys::optics_ui_pop_id(self.ui) };
    }

    /// Shape `text` with the active theme font and return its logical extent.
    /// This is the same measurement seam used by ui widgets during their
    /// intrinsic-size pass.
    pub fn measure_text(&self, text: &str, size: f32) -> TextMetrics {
        let text = cstr(text);
        let opts = sys::optics_ui_text_measure_opts {
            utf8: text.as_ptr(),
            size_px: size.max(1.0),
            weight: 0.0,
        };
        // SAFETY: ui is live for this frame; opts.utf8 is a NUL-terminated
        // string that outlives the call.
        TextMetrics::from_raw(unsafe { sys::optics_ui_text_measure(self.ui, &opts) })
    }

    // ---- clipboard & IME (app-owned editing surfaces) -----------------

    /// Place text on the system clipboard (via the host clipboard
    /// interface, if one was supplied). No-op otherwise.
    pub fn copy(&mut self, text: &str) {
        let opts = sys::optics_ui_copy_opts {
            utf8: text.as_ptr() as *const c_char,
            len: text.len(),
        };
        // SAFETY: ui is live inside a frame; text/len describe a valid range.
        unsafe { sys::optics_ui_copy(self.ui, &opts) };
    }

    /// Ask the host for clipboard text. The payload is delivered into
    /// ui's paste queue (same frame or a later one, platform-dependent);
    /// drain it with [`Frame::take_paste`].
    pub fn request_paste(&mut self) {
        // SAFETY: ui is live inside a frame.
        unsafe { sys::optics_ui_request_paste(self.ui) };
    }

    /// Drain a pending paste payload (one-shot). For app-owned editing
    /// surfaces that render text outside ui widgets; ui text widgets
    /// consume their own paste internally, so only call this while the
    /// app's own surface is the paste target.
    pub fn take_paste(&mut self) -> Option<String> {
        // The C staging buffer caps payloads at 1 MiB (OPTICS_UI_PASTE_MAX).
        let mut buf = vec![0u8; 1024 * 1024];
        let opts = sys::optics_ui_take_paste_opts {
            dst: buf.as_mut_ptr() as *mut c_char,
            cap: buf.len() as u32,
        };
        // SAFETY: ui is live; buf is writable for buf.len() bytes.
        let n = unsafe { sys::optics_ui_take_paste(self.ui, &opts) };
        if n == 0 {
            return None;
        }
        buf.truncate(n as usize);
        // The payload is host-supplied clipboard text; a non-UTF-8 payload
        // is not a paste we can use.
        String::from_utf8(buf).ok()
    }

    /// Report the caret rect of an app-owned editing surface so the
    /// platform IME can position its candidate window at the caret.
    /// UI-space logical pixels.
    pub fn set_caret_rect(&mut self, rect: Rect) {
        // SAFETY: ui is live inside a frame; rect is a value type.
        unsafe { sys::optics_ui_set_caret_rect(self.ui, rect.to_raw()) };
    }

    // ---- containers -------------------------------------------------------

    /// Start a horizontal flex container (row) with a fluent builder.
    #[inline]
    pub fn row(&mut self) -> FlexBuilder<'_, 'frame> {
        FlexBuilder::new(self, false)
    }

    /// Start a vertical flex container (column) with a fluent builder.
    #[inline]
    pub fn column(&mut self) -> FlexBuilder<'_, 'frame> {
        FlexBuilder::new(self, true)
    }

    /// Start a 2D grid container with a fluent builder (ADR-0104).
    #[inline]
    pub fn grid(&mut self, columns: u32) -> GridBuilder<'_, 'frame> {
        GridBuilder::new(self, columns)
    }

    /// Start a multi-line wrapping flex container (flow/wrap) with a fluent builder (ADR-0105).
    #[inline]
    pub fn wrap(&mut self) -> FlexBuilder<'_, 'frame> {
        let mut b = FlexBuilder::new(self, false);
        b.opts.wrap = true;
        b
    }

    /// Start an in-tree stack (layering) container with a fluent builder (ADR-0105).
    #[inline]
    pub fn stack(&mut self) -> StackBuilder<'_, 'frame> {
        StackBuilder::new(self)
    }

    /// Set the number of columns spanned by the next widget or container.
    #[inline]
    pub fn col_span(&mut self, span: u32) {
        unsafe { sys::optics_ui_col_span(self.ui, span) };
    }

    /// Set the number of rows spanned by the next widget or container.
    #[inline]
    pub fn row_span(&mut self, span: u32) {
        unsafe { sys::optics_ui_row_span(self.ui, span) };
    }

    /// Set explicit 1-based grid position (col, row) for the next widget or container.
    #[inline]
    pub fn grid_at(&mut self, col: i32, row: i32) {
        unsafe { sys::optics_ui_grid_at(self.ui, col, row) };
    }

    /// Center `body` on both axes inside a `width` × `height` box.
    ///
    /// ui containers align only on the cross axis ([`LayoutOpts::cross`]);
    /// the main axis always packs from the start, so a bare label inside a
    /// fixed-size column sits at the top and a bare label inside a fixed-size
    /// row sits at the left. `centered` supplies the missing half of the
    /// idiom: a fixed-size row centres the content vertically on its cross
    /// axis, and a matched pair of flexible spacers shares the remaining
    /// main-axis space equally around it. Use it for button labels, icons,
    /// and any fixed-size tile whose content must sit at the optical centre.
    pub fn centered<R>(
        &mut self,
        width: f32,
        height: f32,
        body: impl FnOnce(&mut Frame) -> R,
    ) -> R {
        self.row()
            .width(width)
            .height(height)
            .items_center()
            .show_flat(|frame| {
                frame.flex(1.0);
                frame.spacer(0.0);
                let r = body(frame);
                frame.flex(1.0);
                frame.spacer(0.0);
                r
            })
    }

    /// Open a flex container configured via a [`FlexBuilder`].
    pub(crate) fn show_flex<R>(
        &mut self,
        is_col: bool,
        opts: &LayoutOpts,
        id: Option<(&str, &str)>,
        body: impl FnOnce(&mut Frame) -> R,
    ) -> (Response, R) {
        if let Some((id_str, label_str)) = id {
            let id = cstr(id_str);
            let label = cstr(label_str);
            let raw_opts = sys::optics_ui_pressable_opts {
                box_: sys::optics_ui_box {
                    id: id.as_ptr(),
                    ..Default::default()
                },
                label: label.as_ptr(),
                layout: opts.to_raw(),
                ..Default::default()
            };
            let response =
                Response::from_raw(unsafe { sys::optics_ui_pressable_begin(self.ui, &raw_opts) });
            let result = body(self);
            unsafe { sys::optics_ui_pressable_end(self.ui) };
            (response, result)
        } else if is_col {
            let raw_opts = opts.to_raw();
            unsafe { sys::optics_ui_column_begin(self.ui, &raw_opts) };
            let r = body(self);
            unsafe { sys::optics_ui_close(self.ui) };
            (Response::default(), r)
        } else {
            let raw_opts = opts.to_raw();
            unsafe { sys::optics_ui_row_begin(self.ui, &raw_opts) };
            let r = body(self);
            unsafe { sys::optics_ui_close(self.ui) };
            (Response::default(), r)
        }
    }

    /// A fixed empty gap along the main axis.
    pub fn spacer(&mut self, size: f32) {
        // SAFETY: ui is live for the frame.
        unsafe { sys::optics_ui_spacer(self.ui, size) };
    }

    /// A horizontal rule.
    pub fn separator(&mut self) {
        unsafe {
            sys::optics_ui_separator(self.ui, std::ptr::null());
        }
    }

    /// Fix the next node's size (pass 0 for an axis to keep its intrinsic size).
    pub fn size_next(&mut self, w: f32, h: f32) {
        // SAFETY: ui is live for the frame.
        unsafe { sys::optics_ui_size(self.ui, w, h) };
    }

    /// Give the next node a main-axis grow factor (0 = don't grow). Use it to
    /// fill remaining space (e.g. a flexible spacer, or the main content area
    /// beside a fixed-width sidebar).
    pub fn flex(&mut self, grow: f32) {
        // SAFETY: ui is live for the frame.
        unsafe { sys::optics_ui_flex(self.ui, grow) };
    }

    /// Push a style override for child widgets built within this scope.
    pub fn push_style(&mut self, style: Style) {
        unsafe { sys::optics_ui_push_style(self.ui, style.0) };
    }

    /// Pop the most recent style override pushed with [`Frame::push_style`].
    pub fn pop_style(&mut self) {
        unsafe { sys::optics_ui_pop_style(self.ui) };
    }

    /// Draw an icon glyph at `size` logical pixels. Accepts built-in [`Icon`]
    /// or any custom ID returned by [`Ui::register_icon`].
    pub fn icon(&mut self, id: impl Into<sys::optics_ui_icon_id>, size: f32) {
        let opts = sys::optics_ui_icon_opts {
            id: id.into(),
            size,
            ..Default::default()
        };
        unsafe {
            sys::optics_ui_icon(self.ui, &opts);
        }
    }

    /// Draw a borrowed image into the current layout box.
    ///
    /// # Safety
    ///
    /// `image` must be a live `optics_gfx_image` owned by the device backing this
    /// `Ui`, and it must stay alive (and unmodified) for the duration of the
    /// call. A null pointer is accepted by the C layer as "no image", but
    /// passing a dangling or already-released handle is undefined behaviour.
    pub unsafe fn image(&mut self, image: *mut sys::optics_gfx_image, w: f32, h: f32) {
        let opts = sys::optics_ui_image_opts {
            image,
            width: w,
            height: h,
            ..Default::default()
        };
        unsafe {
            sys::optics_ui_image(self.ui, &opts);
        }
    }

    /// Draw a borrowed image into the current layout box, multiplied by `tint`.
    ///
    /// # Safety
    ///
    /// Same contract as [`Self::image`]: `image` must be a live `optics_gfx_image`
    /// owned by this `Ui`'s device, alive and unmodified for the whole call.
    pub unsafe fn image_tinted(
        &mut self,
        image: *mut sys::optics_gfx_image,
        w: f32,
        h: f32,
        tint: Color,
    ) {
        let opts = sys::optics_ui_image_opts {
            image,
            width: w,
            height: h,
            tint: tint.raw(),
            ..Default::default()
        };
        unsafe {
            sys::optics_ui_image(self.ui, &opts);
        }
    }

    /// Draw an icon-only button. Accepts built-in [`Icon`] or any registered
    /// `optics_ui_icon_id`; `active` highlights the selected/toggled state.
    pub fn icon_button(&mut self, id: impl Into<sys::optics_ui_icon_id>, active: bool) -> bool {
        let opts = sys::optics_ui_button_opts {
            icon: id.into(),
            active,
            ..Default::default()
        };
        unsafe { sys::optics_ui_button(self.ui, &opts).clicked }
    }

    pub fn button(&mut self, label: &str) -> bool {
        self.button_with_variant(label, ButtonVariant::Default)
    }

    pub fn button_with_variant(&mut self, label: &str, variant: ButtonVariant) -> bool {
        let c = cstr(label);
        let opts = sys::optics_ui_button_opts {
            label: c.as_ptr(),
            variant: variant.raw(),
            ..Default::default()
        };
        unsafe { sys::optics_ui_button(self.ui, &opts).clicked }
    }

    pub fn checkbox(&mut self, label: &str, value: &mut bool) -> bool {
        let c = cstr(label);
        let opts = sys::optics_ui_checkbox_opts {
            label: c.as_ptr(),
            value: value as *mut bool,
            ..Default::default()
        };
        unsafe { sys::optics_ui_checkbox(self.ui, &opts).changed }
    }

    pub fn switch(&mut self, label: &str, value: &mut bool) -> bool {
        let c = cstr(label);
        let opts = sys::optics_ui_checkbox_opts {
            label: c.as_ptr(),
            value: value as *mut bool,
            appearance: sys::optics_ui_checkbox_appearance::OPTICS_UI_CHECKBOX_SWITCH,
            ..Default::default()
        };
        unsafe { sys::optics_ui_checkbox(self.ui, &opts).changed }
    }

    pub fn radio(&mut self, label: &str, value: &mut i32, option: i32) -> bool {
        let c = cstr(label);
        let mut is_on = *value == option;
        let opts = sys::optics_ui_checkbox_opts {
            label: c.as_ptr(),
            value: &mut is_on as *mut bool,
            appearance: sys::optics_ui_checkbox_appearance::OPTICS_UI_CHECKBOX_RADIO,
            ..Default::default()
        };
        let r = unsafe { sys::optics_ui_checkbox(self.ui, &opts) };
        if r.changed && is_on {
            *value = option;
            true
        } else {
            false
        }
    }

    pub fn slider(&mut self, label: &str, value: &mut f32, min: f32, max: f32) -> bool {
        self.slider_with(label, value, min, max, &SliderOpts::default())
    }

    pub fn slider_with(
        &mut self,
        label: &str,
        value: &mut f32,
        min: f32,
        max: f32,
        opts: &SliderOpts,
    ) -> bool {
        let c = cstr(label);
        let raw = sys::optics_ui_slider_opts {
            label: c.as_ptr(),
            value: value as *mut f32,
            min,
            max,
            step: if opts.step.is_finite() && opts.step > 0.0 {
                opts.step
            } else {
                0.0
            },
            axis: if opts.vertical {
                sys::optics_ui_axis::OPTICS_UI_COLUMN
            } else {
                sys::optics_ui_axis::OPTICS_UI_ROW
            },
            ..Default::default()
        };
        unsafe { sys::optics_ui_slider(self.ui, &raw).changed }
    }

    pub fn dropdown(&mut self, label: &str, selected: &mut i32, items: &[&str]) -> bool {
        let mut changed = false;
        let menu_id = format!("{}_dropdown_menu", label);
        let curr_label = if *selected >= 0 && (*selected as usize) < items.len() {
            items[*selected as usize]
        } else {
            label
        };
        if self.button(curr_label) {
            self.place_toggle(&menu_id);
        }
        let theme = self.theme();
        let opts = PlaceOpts {
            band: Band::Popup,
            mode: PlaceMode::Anchored,
            transient: true,
            layout: LayoutOpts {
                bg: theme.bg(),
                border: theme.border(),
                border_width: theme.border_width(),
                pad: 4.0,
                gap: 2.0,
                ..Default::default()
            },
            ..Default::default()
        };
        self.place(&menu_id, &opts, |frame| {
            for (i, &item) in items.iter().enumerate() {
                if frame.selectable(item, *selected == i as i32) {
                    *selected = i as i32;
                    changed = true;
                    frame.place_close(&menu_id);
                }
            }
        });
        changed
    }

    pub fn setting_switch(
        &mut self,
        id: &str,
        label: &str,
        description: &str,
        value: &mut bool,
        disabled: bool,
    ) -> Response {
        let (resp, _) = self.row().id(id).cross(Align::Center).show(|frame| {
            frame.column().flex(1.0).show(|frame| {
                frame.label(label);
                if !description.is_empty() {
                    let muted_color = frame.theme().fg().with_alpha(160);
                    frame.push_style(Style::new().with_fg(muted_color));
                    frame.label_with(
                        description,
                        &LabelOpts {
                            size: frame.theme().font_size_sm(),
                            ..Default::default()
                        },
                    );
                    frame.pop_style();
                }
            });
            if !disabled {
                frame.switch(label, value);
            }
        });
        resp
    }

    /// Renders a native segmented control across an array of textual options.
    pub fn segmented(&mut self, id: &str, selected: &mut usize, items: &[&str]) -> bool {
        let seg_items: Vec<patterns::SegmentedItem> = items
            .iter()
            .map(|&s| patterns::SegmentedItem::text(s))
            .collect();
        patterns::SegmentedControl::new(id).show(self, &seg_items, selected)
    }

    /// Renders a settings-row pattern containing a label, description, and segmented control.
    pub fn setting_segmented(
        &mut self,
        id: &str,
        label: &str,
        description: &str,
        selected: &mut usize,
        items: &[&str],
    ) -> bool {
        let mut changed = false;
        self.row().id(id).cross(Align::Center).show(|frame| {
            frame.column().flex(1.0).show(|frame| {
                frame.label(label);
                if !description.is_empty() {
                    let muted_color = frame.theme().fg().with_alpha(160);
                    frame.push_style(Style::new().with_fg(muted_color));
                    frame.label_with(
                        description,
                        &LabelOpts {
                            size: frame.theme().font_size_sm(),
                            ..Default::default()
                        },
                    );
                    frame.pop_style();
                }
            });
            changed = frame.segmented(id, selected, items);
        });
        changed
    }

    pub fn label(&mut self, text: &str) {
        self.label_with(text, &LabelOpts::default());
    }

    pub fn label_with(&mut self, text: &str, opts: &LabelOpts) {
        let c = cstr(text);
        let raw = sys::optics_ui_label_opts {
            box_: sys::optics_ui_box {
                max_width: if opts.max_width.is_finite() && opts.max_width > 0.0 {
                    opts.max_width
                } else {
                    0.0
                },
                ..Default::default()
            },
            text: c.as_ptr(),
            size: if opts.size.is_finite() && opts.size > 0.0 {
                opts.size
            } else {
                0.0
            },
            weight: if opts.weight.is_finite() && opts.weight > 0.0 {
                opts.weight
            } else {
                0.0
            },
            wrap: opts.wrap,
            align: opts.align.raw(),
        };
        unsafe {
            sys::optics_ui_label(self.ui, &raw);
        }
    }

    pub fn heading(&mut self, text: &str, level: i32) {
        let theme = self.theme();
        let size = match level {
            1 => theme.0.font_size_h1,
            2 => theme.0.font_size_h2,
            3 => theme.0.font_size_h3,
            _ => theme.0.font_size_h3,
        };
        self.label_with(
            text,
            &LabelOpts {
                size,
                weight: theme.0.font_weight_bold,
                ..Default::default()
            },
        );
    }

    pub fn selectable(&mut self, label: &str, selected: bool) -> bool {
        let c = cstr(label);
        let opts = sys::optics_ui_selectable_opts {
            label: c.as_ptr(),
            selected,
            ..Default::default()
        };
        unsafe { sys::optics_ui_selectable(self.ui, &opts).clicked }
    }

    pub fn selectable_with_icon(
        &mut self,
        label: &str,
        icon: impl Into<sys::optics_ui_icon_id>,
        selected: bool,
        icon_size: f32,
    ) -> bool {
        let c = cstr(label);
        let opts = sys::optics_ui_selectable_opts {
            label: c.as_ptr(),
            icon: icon.into(),
            selected,
            icon_size,
            ..Default::default()
        };
        unsafe { sys::optics_ui_selectable(self.ui, &opts).clicked }
    }

    pub fn textfield(&mut self, label: &str, buf: &mut TextBuf) -> bool {
        self.textedit(label, buf, &TextEditOpts::default()).changed
    }

    pub fn response(&self) -> Response {
        Response::from_raw(unsafe { sys::optics_ui_get_response(self.ui as *const sys::optics_ui) })
    }

    pub fn scroll<R>(&mut self, id: &str, body: impl FnOnce(&mut Frame) -> R) -> R {
        let c = cstr(id);
        let opts = sys::optics_ui_scroll_opts {
            box_: sys::optics_ui_box {
                id: c.as_ptr(),
                ..Default::default()
            },
            ..Default::default()
        };
        unsafe { sys::optics_ui_scroll_begin(self.ui, &opts) };
        let r = body(self);
        unsafe { sys::optics_ui_scroll_end(self.ui) };
        r
    }

    pub fn place<R>(
        &mut self,
        id: &str,
        opts: &PlaceOpts,
        body: impl FnOnce(&mut Frame) -> R,
    ) -> Option<R> {
        let c = cstr(id);
        let mut raw = opts.to_raw();
        raw.box_.id = c.as_ptr();
        let open = unsafe { sys::optics_ui_place_begin(self.ui, &raw) };
        if open {
            let r = body(self);
            unsafe { sys::optics_ui_place_end(self.ui) };
            Some(r)
        } else {
            None
        }
    }

    pub fn place_open(&mut self, id: &str) {
        let c = cstr(id);
        unsafe { sys::optics_ui_place_open(self.ui, c.as_ptr()) };
    }

    pub fn place_close(&mut self, id: &str) {
        let c = cstr(id);
        unsafe { sys::optics_ui_place_close(self.ui, c.as_ptr()) };
    }

    pub fn place_toggle(&mut self, id: &str) {
        let c = cstr(id);
        unsafe { sys::optics_ui_place_toggle(self.ui, c.as_ptr()) };
    }

    pub fn place_is_open(&self, id: &str) -> bool {
        let c = cstr(id);
        unsafe { sys::optics_ui_place_is_open(self.ui as *const sys::optics_ui, c.as_ptr()) }
    }

    pub fn place_close_all(&mut self) {
        unsafe { sys::optics_ui_place_close_all(self.ui) };
    }

    pub fn node_bounds(&self, id: &str) -> Option<Rect> {
        let c = cstr(id);
        let raw_id = unsafe { sys::optics_ui_current_id(self.ui, c.as_ptr()) };
        if raw_id == 0 {
            return None;
        }
        let node = unsafe { sys::optics_ui_find(self.ui, raw_id) };
        if node.is_null() {
            return None;
        }
        Some(Rect::from_raw(unsafe { sys::optics_ui_node_bounds(node) }))
    }

    pub fn consume_key(&mut self, key: i32) {
        unsafe { sys::optics_ui_consume_key(self.ui, key) };
    }

    pub fn dnd_source(&mut self, id: &str, text: &str, actions: u32) -> bool {
        let c_id = cstr(id);
        let raw_id = unsafe { sys::optics_ui_current_id(self.ui, c_id.as_ptr()) };
        let c_text = cstr(text);
        let desc = sys::optics_ui_dnd_source_desc {
            id: raw_id,
            text: c_text.as_ptr(),
            text_len: text.len(),
            actions,
            preview_rect: sys::optics_gfx_rect {
                x: 0.0,
                y: 0.0,
                w: 0.0,
                h: 0.0,
            },
        };
        unsafe { sys::optics_ui_dnd_source(self.ui, &desc) }
    }

    pub fn dnd_drop_target(&mut self, id: &str, accepted_actions: u32) -> Option<DndDropInfo> {
        let c_id = cstr(id);
        let raw_id = unsafe { sys::optics_ui_current_id(self.ui, c_id.as_ptr()) };
        let mut info = sys::optics_ui_dnd_drop_info {
            is_hovered: false,
            is_dropped: false,
            drop_pos: sys::optics_gfx_point { x: 0.0, y: 0.0 },
            action: 0,
        };
        let dnd_opts = sys::optics_ui_dnd_drop_target_opts {
            id: raw_id,
            accepted_actions,
            out_info: &mut info,
        };
        let hit = unsafe { sys::optics_ui_dnd_drop_target(self.ui, &dnd_opts) };
        if hit || info.is_hovered || info.is_dropped {
            let payload = if info.is_dropped {
                let mut buf = [0u8; 1024];
                let take_opts = sys::optics_ui_take_drop_opts {
                    dst: buf.as_mut_ptr() as *mut std::os::raw::c_char,
                    cap: buf.len() as u32,
                };
                let len = unsafe { sys::optics_ui_take_drop(self.ui, &take_opts) };
                if len > 0 {
                    Some(String::from_utf8_lossy(&buf[..len as usize]).into_owned())
                } else {
                    None
                }
            } else {
                None
            };
            Some(DndDropInfo {
                is_hovered: info.is_hovered,
                is_dropped: info.is_dropped,
                action: info.action,
                payload,
            })
        } else {
            None
        }
    }

    pub fn textedit(
        &mut self,
        label: &str,
        buf: &mut TextBuf,
        opts: &TextEditOpts<'_>,
    ) -> Response {
        let c = cstr(label);
        let placeholder_c = opts.placeholder.map(cstr);
        let raw = sys::optics_ui_textedit_opts {
            box_: sys::optics_ui_box {
                id: c.as_ptr(),
                ..Default::default()
            },
            buf: buf.as_mut_ptr(),
            cap: buf.cap(),
            placeholder: placeholder_c
                .as_ref()
                .map_or(std::ptr::null(), |s| s.as_ptr()),
            multiline: opts.multiline,
            rows: opts.rows,
            password: opts.password,
            select_all_on_focus: opts.select_all_on_focus,
            readonly: opts.readonly,
            accept_tab: opts.accept_tab,
            submit_on_enter: opts.submit_on_enter,
        };
        Response::from_raw(unsafe { sys::optics_ui_textedit(self.ui, &raw) })
    }

    pub fn set_cursor_hint(&mut self, hint: CursorHint) {
        unsafe { sys::optics_ui_set_cursor_hint(self.ui, hint.raw()) };
    }

    pub fn textedit_set_caret(&mut self, label: &str, caret: u32) {
        let c = cstr(label);
        let opts = sys::optics_ui_textedit_set_caret_opts {
            label: c.as_ptr(),
            caret,
        };
        unsafe { sys::optics_ui_textedit_set_caret(self.ui, &opts) };
    }

    pub fn textedit_set_selection(&mut self, label: &str, anchor: u32, caret: u32) {
        let c = cstr(label);
        let opts = sys::optics_ui_textedit_set_selection_opts {
            label: c.as_ptr(),
            sel_start: anchor,
            sel_end: caret,
        };
        unsafe { sys::optics_ui_textedit_set_selection(self.ui, &opts) };
    }
}

/// A fixed-capacity, NUL-terminated UTF-8 edit buffer for [`Frame::textfield`].
///
/// ui edits the buffer in place (C string semantics), so Rust's `String`
/// can't back it directly. `TextBuf` owns a `Vec<u8>` of fixed capacity and
/// hands the C side a stable pointer.
pub struct TextBuf {
    bytes: Vec<u8>,
}

impl TextBuf {
    /// A new buffer of `capacity` bytes (including the NUL terminator),
    /// pre-filled with `initial`.
    pub fn new(capacity: usize, initial: &str) -> TextBuf {
        assert!(capacity >= 1, "capacity must leave room for a NUL");
        let mut bytes = vec![0u8; capacity];
        let src = initial.as_bytes();
        let n = src.len().min(capacity - 1);
        bytes[..n].copy_from_slice(&src[..n]);
        TextBuf { bytes }
    }

    fn as_mut_ptr(&mut self) -> *mut c_char {
        self.bytes.as_mut_ptr() as *mut c_char
    }

    fn cap(&self) -> usize {
        self.bytes.len()
    }

    /// Replace the buffer contents with `text`, preserving the capacity.
    /// Truncated to `capacity - 1` bytes (room for the NUL terminator). Use to
    /// reset a persistent buffer the host owns alongside a textfield — ui
    /// keeps its cursor clamped to the new length on the next frame. Follow
    /// with [`Frame::textedit_set_caret`] when the caret should land
    /// somewhere specific instead (e.g. the end of a Tab completion).
    pub fn set(&mut self, text: &str) {
        let cap = self.bytes.len();
        if cap == 0 {
            return;
        }
        let n = text.len().min(cap - 1);
        // Wipe first so a shorter replacement can't leave stale tail bytes
        // before the new NUL terminator.
        self.bytes.fill(0);
        self.bytes[..n].copy_from_slice(&text.as_bytes()[..n]);
        self.bytes[n] = 0;
    }

    /// The current contents up to the NUL terminator, as a `&str` (lossy if the
    /// C side ever wrote invalid UTF-8, which the text widgets do not).
    pub fn as_str(&self) -> std::borrow::Cow<'_, str> {
        let end = self
            .bytes
            .iter()
            .position(|&b| b == 0)
            .unwrap_or(self.bytes.len());
        String::from_utf8_lossy(&self.bytes[..end])
    }
}

/// A ui operation failed.
///
/// Carries the *gfx diagnostic*, not just the result code. The code alone
/// (`OPTICS_GFX_ERROR_INVALID_ARGUMENT`) cannot distinguish a degenerate rect from an
/// out-of-bounds one from a double-begun pass, and every one of those needs a
/// different fix. gfx records which entry point failed, at which source line,
/// with a human-readable message, and discards it unless the caller reads it at
/// the failure site — so the read has to happen here, in the constructor, and
/// the detail has to travel with the error.
///
/// This is why the type is not `#[error("... {0:?}")]` over a bare
/// `optics_gfx_result`: that shape throws the diagnostic away at exactly the moment it
/// is the only thing that can explain the failure.
#[derive(Debug)]
pub enum Error {
    /// `optics_ui_create` returned a non-OK `optics_gfx_result`.
    Create(GfxDiagnostic),
    /// Snapshot publication or execution returned a non-OK `optics_gfx_result`.
    Render(GfxDiagnostic),
}

/// The gfx diagnostic behind a failed ui call.
///
/// `optics_ui_sys` binds the enum from its own build of the C headers, so its
/// `optics_gfx_result` is a nominally distinct type from `optics_gfx::sys`'s even though both
/// mirror the same C enum. Bridging on the discriminant would need a
/// `TryFrom<i32>` that bindgen does not emit, and would be a conversion whose
/// only purpose is to satisfy the type checker. Carrying the code plus gfx's
/// own rendered diagnostic instead keeps the information and skips the dance: the
/// message is what a human needs, and it comes from the same thread-local slot
/// either way.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GfxDiagnostic {
    /// The `optics_gfx_result` discriminant, as gfx reported it.
    pub code: i32,
    /// `"{OPTICS_GFX_ERROR_...}"` — the enum's own name, so logs stay greppable.
    pub code_name: String,
    /// gfx entry point that failed, e.g. `"optics_gfx_canvas_begin"`.
    pub function: Option<String>,
    /// Source file inside gfx, and the line in it.
    pub file: Option<String>,
    pub line: i32,
    /// Human-readable detail: which argument, which bound, which state.
    pub message: Option<String>,
    /// Backend result (e.g. `VkResult` as i32) when relevant.
    pub backend_code: i32,
}

impl GfxDiagnostic {
    /// Build a diagnostic from a raw result, for tests and for any binding that
    /// needs to render a failure it captured itself.
    ///
    /// Public because the *contract* — losing the detail is a silent regression —
    /// can only be pinned by a test, and a test has no access to the private
    /// `Error` constructors.
    #[doc(hidden)]
    pub fn for_test(rc: sys::optics_gfx_result) -> Self {
        Self::read(rc)
    }

    fn read(rc: sys::optics_gfx_result) -> Self {
        /* Read the thread-local slot *here*, at the failure site: gfx overwrites
         * it on the next failing call, so a later read would describe a different
         * error — or none. `last()` returns `None` when the slot is empty or
         * holds an unrelated code, which is the "no detail recorded" case. */
        match optics_gfx::ErrorInfo::last() {
            Some(info) => GfxDiagnostic {
                code: rc as i32,
                code_name: format!("{rc:?}"),
                function: info.function,
                file: info.file,
                line: info.line,
                message: info.message,
                backend_code: info.backend_code,
            },
            None => GfxDiagnostic {
                code: rc as i32,
                code_name: format!("{rc:?}"),
                function: None,
                file: None,
                line: 0,
                message: None,
                backend_code: 0,
            },
        }
    }
}

impl std::fmt::Display for GfxDiagnostic {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}", self.code_name)?;
        if let Some(function) = &self.function {
            write!(f, " at {function}")?;
            if let Some(file) = &self.file {
                write!(f, " ({file}:{})", self.line)?;
            }
        }
        if let Some(message) = &self.message {
            write!(f, ": {message}")?;
        }
        Ok(())
    }
}

impl Error {
    /// Build from a gfx result, reading the thread-local diagnostic while it
    /// still describes *this* call.
    ///
    /// `optics_gfx::ErrorInfo::last_if` declines a stale diagnostic whose code belongs
    /// to a different call, so a missing detail means "gfx did not record one
    /// for this code" rather than "the detail was lost on the way here".
    pub(crate) fn from_result(ctor: fn(GfxDiagnostic) -> Self, rc: sys::optics_gfx_result) -> Self {
        ctor(GfxDiagnostic::read(rc))
    }
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Error::Create(info) => write!(f, "optics_ui_create failed: {info}"),
            Error::Render(info) => write!(f, "snapshot operation failed: {info}"),
        }
    }
}

impl std::error::Error for Error {}

/// The built-in default skin for a widget kind (ADR-0059). Useful for
/// wrapping: a custom skin can call the default to keep the stock chrome
/// and then add its own. Returns `None` for kinds outside the enum.
pub fn default_skin(kind: WidgetKind) -> SkinFn {
    // SAFETY: pure table lookup.
    unsafe { sys::optics_ui_default_skin(kind) }
}

/// Push a filled rect from inside a skin (ADR-0059). `rel` is node-local;
/// a zero w/h spans the full node box.
///
/// # Safety
/// `ui` and `node` must be the values the skin was called with, during
/// that call.
/// Borrow a node's retained skin scratch (ADR-0061): four floats, zeroed on
/// the node's first touch, living and dying with the node. Mechanism, not
/// animation — a caller-owned skin stores its own state (a spring's
/// position/velocity) here; the library never integrates anything.
///
/// # Safety
/// `node` must be a live ui node obtained inside the current frame (e.g.
/// the node passed to a skin callback); the returned pointer is valid until
/// the node is reclaimed by the store GC.
pub unsafe fn skin_scratch(ui: *mut sys::optics_ui, node: *mut sys::optics_ui_node) -> *mut f32 {
    // SAFETY: forwarded contract — the caller guarantees a live node.
    unsafe { sys::optics_ui_skin_scratch(ui, node) }
}
/// Push a filled rounded rect from inside a skin.
///
/// # Safety
/// `ui` and `node` must be the values the skin was called with, during
/// that call.
pub unsafe fn skin_rect(
    ui: *mut sys::optics_ui,
    node: *mut sys::optics_ui_node,
    rel: Rect,
    color: Color,
    corner_radii: impl Into<CornerRadii>,
    corner_curvature: f32,
) {
    let opts = sys::optics_ui_skin_rect_opts {
        node,
        rel: rel.to_raw(),
        color: color.raw(),
        corner_radii: types::raw_corner_radii(corner_radii.into()),
        corner_curvature: types::clamp_curvature(corner_curvature),
    };
    // SAFETY: forwarded contract from the caller.
    unsafe { sys::optics_ui_skin_rect(ui, &opts) };
}

/// Push a border stroke from inside a skin. Same contract as [`skin_rect`].
///
/// # Safety
/// `ui` and `node` must be the values the skin was called with, during
/// that call.
pub unsafe fn skin_border(
    ui: *mut sys::optics_ui,
    node: *mut sys::optics_ui_node,
    rel: Rect,
    color: Color,
    corner_radii: impl Into<CornerRadii>,
    corner_curvature: f32,
    width: f32,
) {
    let opts = sys::optics_ui_skin_border_opts {
        node,
        rel: rel.to_raw(),
        color: color.raw(),
        corner_radii: types::raw_corner_radii(corner_radii.into()),
        corner_curvature: types::clamp_curvature(corner_curvature),
        width,
    };
    // SAFETY: forwarded contract from the caller.
    unsafe { sys::optics_ui_skin_border(ui, &opts) };
}

/// Push text from inside a skin. A negative `rel.w`/`rel.h` centres the
/// run in the resolved node rect at render time. Same contract as
/// [`skin_rect`].
///
/// # Safety
/// `ui` and `node` must be the values the skin was called with, during
/// that call; ui copies the text into its frame arena.
pub unsafe fn skin_text(
    ui: *mut sys::optics_ui,
    node: *mut sys::optics_ui_node,
    rel: Rect,
    color: Color,
    text: &str,
    size_px: f32,
    weight: f32,
) {
    let c = cstr(text);
    let opts = sys::optics_ui_skin_text_opts {
        node,
        rel: rel.to_raw(),
        color: color.raw(),
        utf8: c.as_ptr(),
        size_px,
        weight,
    };
    // SAFETY: forwarded contract from the caller; c outlives the call.
    unsafe { sys::optics_ui_skin_text(ui, &opts) };
}

/// Push a vector icon glyph from inside a skin. `icon` is a raw
/// `optics_ui_icon_id`, so runtime-registered glyphs ([`Ui::register_icon`])
/// work alongside the built-in [`Icon`] set. Same contract as
/// [`skin_rect`].
///
/// # Safety
/// `ui` and `node` must be the values the skin was called with, during
/// that call.
pub unsafe fn skin_icon(
    ui: *mut sys::optics_ui,
    node: *mut sys::optics_ui_node,
    rel: Rect,
    color: Color,
    stroke: f32,
    icon: sys::optics_ui_icon_id,
) {
    let opts = sys::optics_ui_skin_icon_opts {
        node,
        rel: rel.to_raw(),
        color: color.raw(),
        stroke,
        icon,
    };
    // SAFETY: forwarded contract from the caller.
    unsafe { sys::optics_ui_skin_icon(ui, &opts) };
}

/// Push a nested logical clip from inside a skin (e.g. per-cell clips in a
/// table skin). Balanced with [`skin_clip_pop`]; the rect is intersected
/// with the enclosing clip at render time. Same contract as [`skin_rect`].
///
/// # Safety
/// `ui` and `node` must be the values the skin was called with, during
/// that call.
pub unsafe fn skin_clip_push(ui: *mut sys::optics_ui, node: *mut sys::optics_ui_node, rel: Rect) {
    let opts = sys::optics_ui_skin_clip_push_opts {
        node,
        rel: rel.to_raw(),
    };
    // SAFETY: forwarded contract from the caller.
    unsafe { sys::optics_ui_skin_clip_push(ui, &opts) };
}

/// Pop a clip pushed with [`skin_clip_push`]. Same contract as
/// [`skin_rect`].
///
/// # Safety
/// `ui` and `node` must be the values the skin was called with, during
/// that call, and every pop must balance an earlier push.
pub unsafe fn skin_clip_pop(ui: *mut sys::optics_ui, node: *mut sys::optics_ui_node) {
    // SAFETY: forwarded contract from the caller.
    unsafe { sys::optics_ui_skin_clip_pop(ui, node) };
}

/// The ui version string reported by the linked C library.
pub fn version() -> &'static str {
    // SAFETY: optics_ui_version_string returns a static NUL-terminated string.
    let p = unsafe { sys::optics_ui_version_string() };
    unsafe { std::ffi::CStr::from_ptr(p) }
        .to_str()
        .unwrap_or("<invalid>")
}

/// The linked library's packed version number — identical layout to
/// gfx's (`major << 16 | minor << 8 | patch`).
pub fn version_number() -> u32 {
    unsafe { sys::optics_ui_version_number() }
}

/// True when the linked library can stand in for the one compiled
/// against: same major, and its (minor, patch) >= the requested pair.
pub fn version_check(major: i32, minor: i32, patch: i32) -> bool {
    unsafe { sys::optics_ui_version_check(major, minor, patch) }
}

/// A zero-allocation fluent builder for Flexbox containers (rows, columns, and
/// custom layout boxes).
///
/// Combines layout options, visual styling (background, border, radius), and
/// optional interactive target semantics (`id` / pressable) into a single,
/// unified container primitive.
pub struct FlexBuilder<'a, 'frame> {
    frame: &'a mut Frame<'frame>,
    is_col: bool,
    opts: LayoutOpts,
    id: Option<(&'a str, &'a str)>,
}

impl<'a, 'frame> FlexBuilder<'a, 'frame> {
    pub(crate) fn new(frame: &'a mut Frame<'frame>, is_col: bool) -> Self {
        Self {
            frame,
            is_col,
            opts: LayoutOpts::new(),
            id: None,
        }
    }

    /// Set main-axis gap between children in logical px.
    #[inline]
    pub fn gap(mut self, px: f32) -> Self {
        self.opts.gap = px;
        self
    }

    /// Set uniform padding in logical px.
    #[inline]
    pub fn pad(mut self, px: f32) -> Self {
        self.opts.pad = px;
        self
    }

    /// Center children along the cross-axis.
    #[inline]
    pub fn items_center(mut self) -> Self {
        self.opts.cross = Align::Center;
        self
    }

    /// Set cross-axis alignment.
    #[inline]
    pub fn cross(mut self, align: Align) -> Self {
        self.opts.cross = align;
        self
    }

    /// Set flex grow factor.
    #[inline]
    pub fn flex(mut self, factor: f32) -> Self {
        self.opts.flex = factor;
        self
    }

    /// Set background fill color.
    #[inline]
    pub fn bg(mut self, color: Color) -> Self {
        self.opts.bg = color;
        self
    }

    /// Set independent per-corner radii `(tl, tr, br, bl)` in logical px.
    /// Accepts `f32` (uniform), `[f32; 4]`, or [`CornerRadii`].
    #[inline]
    pub fn corner_radii(mut self, radii: impl Into<CornerRadii>) -> Self {
        self.opts.corner_radii = radii.into();
        self
    }

    /// Set continuous corner curvature (`0.0` = circular arc, `1.0` = G2 squircle).
    #[inline]
    pub fn corner_curvature(mut self, curvature: f32) -> Self {
        self.opts.corner_curvature = curvature;
        self
    }

    /// Set border stroke color.
    #[inline]
    pub fn border(mut self, color: Color) -> Self {
        self.opts.border = color;
        self
    }

    /// Set border stroke width in logical px.
    #[inline]
    pub fn border_width(mut self, w: f32) -> Self {
        self.opts.border_width = w;
        self
    }

    /// Set explicit width in logical px.
    #[inline]
    pub fn width(mut self, w: f32) -> Self {
        self.opts.width = w;
        self
    }

    /// Set explicit height in logical px.
    #[inline]
    pub fn height(mut self, h: f32) -> Self {
        self.opts.height = h;
        self
    }

    /// Set minimum resolved width in logical px.
    #[inline]
    pub fn min_width(mut self, w: f32) -> Self {
        self.opts.min_width = w;
        self
    }

    /// Set minimum resolved height in logical px.
    #[inline]
    pub fn min_height(mut self, h: f32) -> Self {
        self.opts.min_height = h;
        self
    }

    /// Set maximum resolved width in logical px.
    #[inline]
    pub fn max_width(mut self, w: f32) -> Self {
        self.opts.max_width = w;
        self
    }

    /// Set maximum resolved height in logical px.
    #[inline]
    pub fn max_height(mut self, h: f32) -> Self {
        self.opts.max_height = h;
        self
    }

    /// Set columns spanned in an enclosing 2D grid.
    #[inline]
    pub fn col_span(mut self, span: i32) -> Self {
        self.opts.col_span = span;
        self
    }

    /// Set rows spanned in an enclosing 2D grid.
    #[inline]
    pub fn row_span(mut self, span: i32) -> Self {
        self.opts.row_span = span;
        self
    }

    /// Set explicit 1-based grid position (col, row).
    #[inline]
    pub fn grid_at(mut self, col: i32, row: i32) -> Self {
        self.opts.grid_col = col;
        self.opts.grid_row = row;
        self
    }

    /// Apply all options from a [`LayoutOpts`] descriptor.
    #[inline]
    pub fn with_opts(mut self, opts: &LayoutOpts) -> Self {
        self.opts = *opts;
        self
    }

    /// Assign an interaction ID to turn this container into an interactive
    /// click/hover target.
    #[inline]
    pub fn id(mut self, id: &'a str) -> Self {
        self.id = Some((id, ""));
        self
    }

    /// Assign an interaction ID and accessibility label.
    #[inline]
    pub fn id_label(mut self, id: &'a str, label: &'a str) -> Self {
        self.id = Some((id, label));
        self
    }

    /// Enable or disable multi-line wrapping (ADR-0105).
    #[inline]
    pub fn wrapping(mut self, enabled: bool) -> Self {
        self.opts.wrap = enabled;
        self
    }

    /// Set vertical gap between wrapped lines in logical px (ADR-0105).
    #[inline]
    pub fn row_gap(mut self, gap: f32) -> Self {
        self.opts.row_gap = gap;
        self
    }

    /// Open the container scope, execute `body` with the child frame, and
    /// return a tuple of `(Response, R)`.
    pub fn show<R>(self, body: impl FnOnce(&mut Frame) -> R) -> (Response, R) {
        self.frame.show_flex(self.is_col, &self.opts, self.id, body)
    }

    /// Open the container scope, execute `body`, and return only the closure result `R`.
    #[inline]
    pub fn show_flat<R>(self, body: impl FnOnce(&mut Frame) -> R) -> R {
        self.show(body).1
    }

    /// Render an empty container node and return its interaction response.
    #[inline]
    pub fn empty(self) -> Response {
        self.show(|_| ()).0
    }
}

/// Fluent builder for in-tree layering stack containers (ADR-0105).
pub struct StackBuilder<'a, 'frame> {
    frame: &'a mut Frame<'frame>,
    opts: LayoutOpts,
}

impl<'a, 'frame> StackBuilder<'a, 'frame> {
    #[inline]
    pub fn new(frame: &'a mut Frame<'frame>) -> Self {
        Self {
            frame,
            opts: LayoutOpts::default(),
        }
    }

    #[inline]
    pub fn pad(mut self, p: f32) -> Self {
        self.opts.pad = p;
        self
    }

    #[inline]
    pub fn bg(mut self, color: Color) -> Self {
        self.opts.bg = color;
        self
    }

    #[inline]
    pub fn corner_radii(mut self, radii: impl Into<CornerRadii>) -> Self {
        self.opts.corner_radii = radii.into();
        self
    }

    #[inline]
    pub fn corner_curvature(mut self, curvature: f32) -> Self {
        self.opts.corner_curvature = curvature;
        self
    }

    #[inline]
    pub fn border(mut self, color: Color) -> Self {
        self.opts.border = color;
        self
    }

    #[inline]
    pub fn border_width(mut self, w: f32) -> Self {
        self.opts.border_width = w;
        self
    }

    #[inline]
    pub fn width(mut self, w: f32) -> Self {
        self.opts.width = w;
        self
    }

    #[inline]
    pub fn height(mut self, h: f32) -> Self {
        self.opts.height = h;
        self
    }

    #[inline]
    pub fn with_opts(mut self, opts: &LayoutOpts) -> Self {
        self.opts = *opts;
        self
    }

    pub fn show<R>(self, body: impl FnOnce(&mut Frame) -> R) -> R {
        let raw = self.opts.to_raw();
        unsafe { sys::optics_ui_stack_begin(self.frame.ui, &raw) };
        let r = body(self.frame);
        unsafe { sys::optics_ui_stack_end(self.frame.ui) };
        r
    }
}

/// Fluent builder for 2D grid containers (ADR-0104).
pub struct GridBuilder<'a, 'frame> {
    frame: &'a mut Frame<'frame>,
    opts: sys::optics_ui_grid_opts,
}

impl<'a, 'frame> GridBuilder<'a, 'frame> {
    #[inline]
    fn finite_pos(v: f32) -> f32 {
        if v.is_finite() && v > 0.0 { v } else { 0.0 }
    }

    pub(crate) fn new(frame: &'a mut Frame<'frame>, columns: u32) -> Self {
        let mut opts: sys::optics_ui_grid_opts = unsafe { std::mem::zeroed() };
        opts.columns = columns.max(1) as std::os::raw::c_int;
        opts.cross = sys::optics_ui_align::OPTICS_UI_STRETCH;
        Self { frame, opts }
    }

    /// Set horizontal gap between columns in logical px.
    #[inline]
    pub fn col_gap(mut self, px: f32) -> Self {
        self.opts.col_gap = Self::finite_pos(px);
        self
    }

    /// Set vertical gap between rows in logical px.
    #[inline]
    pub fn row_gap(mut self, px: f32) -> Self {
        self.opts.row_gap = Self::finite_pos(px);
        self
    }

    /// Set both column and row gaps to the same value.
    #[inline]
    pub fn gap(mut self, px: f32) -> Self {
        let g = Self::finite_pos(px);
        self.opts.col_gap = g;
        self.opts.row_gap = g;
        self
    }

    /// Set uniform container padding in logical px.
    #[inline]
    pub fn pad(mut self, px: f32) -> Self {
        self.opts.pad = Self::finite_pos(px);
        self
    }

    /// Set uniform row height (0 = content-measured).
    #[inline]
    pub fn row_height(mut self, px: f32) -> Self {
        self.opts.row_height = Self::finite_pos(px);
        self
    }

    /// Set cross-axis alignment.
    #[inline]
    pub fn cross(mut self, align: Align) -> Self {
        self.opts.cross = align.raw();
        self
    }

    /// Set cell horizontal alignment.
    #[inline]
    pub fn align(mut self, align: Align) -> Self {
        self.opts.align = align.raw();
        self
    }

    /// Set columns spanned in an enclosing grid.
    #[inline]
    pub fn col_span(mut self, span: i32) -> Self {
        self.opts.box_.col_span = span.max(0);
        self
    }

    /// Set rows spanned in an enclosing grid.
    #[inline]
    pub fn row_span(mut self, span: i32) -> Self {
        self.opts.box_.row_span = span.max(0);
        self
    }

    /// Set explicit 1-based grid position (col, row).
    #[inline]
    pub fn grid_at(mut self, col: i32, row: i32) -> Self {
        self.opts.box_.grid_col = col.max(0);
        self.opts.box_.grid_row = row.max(0);
        self
    }

    /// Set explicit width in logical px.
    #[inline]
    pub fn width(mut self, w: f32) -> Self {
        self.opts.box_.width = Self::finite_pos(w);
        self
    }

    /// Set explicit height in logical px.
    #[inline]
    pub fn height(mut self, h: f32) -> Self {
        self.opts.box_.height = Self::finite_pos(h);
        self
    }

    /// Set flex grow factor.
    #[inline]
    pub fn flex(mut self, factor: f32) -> Self {
        self.opts.box_.flex = Self::finite_pos(factor);
        self
    }

    /// Set background fill color.
    #[inline]
    pub fn bg(mut self, color: Color) -> Self {
        self.opts.bg = color.raw();
        self
    }

    /// Set independent per-corner radii `(tl, tr, br, bl)` in logical px.
    /// Accepts `f32` (uniform), `[f32; 4]`, or [`CornerRadii`].
    #[inline]
    pub fn corner_radii(mut self, radii: impl Into<CornerRadii>) -> Self {
        self.opts.corner_radii = types::raw_corner_radii(radii.into());
        self
    }

    /// Set continuous corner curvature (`0.0` = circular arc, `1.0` = G2 squircle).
    #[inline]
    pub fn corner_curvature(mut self, curvature: f32) -> Self {
        self.opts.corner_curvature = types::clamp_curvature(curvature);
        self
    }

    /// Set border stroke color.
    #[inline]
    pub fn border(mut self, color: Color) -> Self {
        self.opts.border = color.raw();
        self
    }

    /// Set border stroke width in logical px.
    #[inline]
    pub fn border_width(mut self, width: f32) -> Self {
        self.opts.border_width = Self::finite_pos(width);
        self
    }

    /// Execute the body within the grid container.
    pub fn show<R>(self, body: impl FnOnce(&mut Frame) -> R) -> R {
        unsafe { sys::optics_ui_grid_begin(self.frame.ui, &self.opts) };
        let r = body(self.frame);
        unsafe { sys::optics_ui_grid_end(self.frame.ui) };
        r
    }
}

/// Build a `CString`, panicking on an interior NUL (a programming error in the
/// caller's literal, not a runtime condition).
fn cstr(s: &str) -> CString {
    CString::new(s).expect("string passed to a widget contains an interior NUL")
}
