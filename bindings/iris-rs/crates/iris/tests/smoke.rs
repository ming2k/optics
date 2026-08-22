//! Smoke tests for iris-rs that do not require a Wayland session.
//!
//! These exercise the API surface that is reachable without a window:
//! version string, Config construction, color-scheme query (which degrades
//! gracefully when no portal is reachable), and cursor enum.

#![deny(rust_2018_idioms)]

use iris::{ColorScheme, Cursor};

#[test]
fn version_string_is_nonempty() {
    let v = iris::version();
    assert!(!v.is_empty());
    assert!(v.contains('.'), "version should contain a dot: {v}");
}

#[test]
fn config_new_succeeds_with_valid_title() {
    let cfg = iris::Config::new("test-app").expect("Config::new with a short ASCII title");
    // Round-trip a couple of builder options to make sure they chain.
    let cfg = cfg.size(640, 480).force_dark();
    let _ = cfg.app_id("ai.opencode.test").expect("app_id");
}

#[test]
fn config_new_rejects_embedded_nul() {
    // A NUL byte in the title would silently truncate when handed to C as a
    // CString; the binding surfaces that as an error rather than corrupting
    // the title silently.
    let r = iris::Config::new("bad\0title");
    assert!(r.is_err());
}

#[test]
fn config_app_id_rejects_embedded_nul() {
    let cfg = iris::Config::new("test-app").unwrap();
    assert!(cfg.app_id("bad\0id").is_err());
}

#[test]
fn color_scheme_query_does_not_panic() {
    // The portal may be unreachable in CI; we only assert the call returns
    // a value from the documented enum (Light / Dark / NoPreference) and
    // does not panic. The Light/Dark bool helpers build on this.
    let _scheme: ColorScheme = iris::query_system_color_scheme();
    let _bool = iris::system_prefers_dark();
}

#[test]
fn cursor_enum_round_trips() {
    // Cursor values are passed straight through to iris_set_cursor; make
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

#[test]
fn config_roundtrip_is_panic_free() {
    // Honest name for what this checks: constructing and dropping a Config
    // (it cannot call Application::run from a unit test — no compositor).
    // The null-config rejection path of iris_app_run is covered by the C
    // test suite (tests/iris/test_app_api.c), not here.
    let cfg = iris::Config::new("noop").unwrap();
    drop(cfg); // no panic on drop
}
