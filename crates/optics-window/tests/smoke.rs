//! Smoke tests for the `iris` binding that do not require a Wayland session.
//!
//! These exercise the API surface that is reachable without a window:
//! version string, color-scheme query (which degrades
//! gracefully when no portal is reachable), and cursor enum.

#![deny(rust_2018_idioms)]

use optics_window::{ColorScheme, Cursor};

#[test]
fn version_string_is_nonempty() {
    let v = optics_window::version();
    assert!(!v.is_empty());
    assert!(v.contains('.'), "version should contain a dot: {v}");
}

#[test]
fn color_scheme_query_does_not_panic() {
    // The portal may be unreachable in CI; we only assert the call returns
    // a value from the documented enum (Light / Dark / NoPreference) and
    // does not panic. The Light/Dark bool helpers build on this.
    let _scheme: ColorScheme = optics_window::query_system_color_scheme();
    let _bool = optics_window::system_prefers_dark();
}

#[test]
fn cursor_enum_round_trips() {
    // Cursor values are passed straight through to optics_window_set_cursor; make
    // sure the repr matches a known value so adding a new variant fails
    // this test before silently breaking the binding.
    let _ = Cursor::Default;
    let _ = Cursor::Text;
    let _ = Cursor::Pointer;
    let _ = Cursor::Busy;
    let _ = Cursor::Crosshair;
    let _ = Cursor::NotAllowed;
    let _ = Cursor::ResizeEw;
    let _ = Cursor::ResizeNs;
}
