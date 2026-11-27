//! Smoke tests for the `app` binding that do not require a window:
//! Config construction and validation.

#![deny(rust_2018_idioms)]

#[test]
fn config_new_succeeds_with_valid_title() {
    let cfg = optics_app::Config::new("test-app").expect("Config::new with a short ASCII title");
    // Round-trip a couple of builder options to make sure they chain.
    let cfg = cfg.size(640, 480).force_dark();
    let _ = cfg.app_id("ai.opencode.test").expect("app_id");
}

#[test]
fn config_new_rejects_embedded_nul() {
    // A NUL byte in the title would silently truncate when handed to C as a
    // CString; the binding surfaces that as an error rather than corrupting
    // the title silently.
    let r = optics_app::Config::new("bad\0title");
    assert!(r.is_err());
}

#[test]
fn config_app_id_rejects_embedded_nul() {
    let cfg = optics_app::Config::new("test-app").unwrap();
    assert!(cfg.app_id("bad\0id").is_err());
}

#[test]
fn config_roundtrip_is_panic_free() {
    // Honest name for what this checks: constructing and dropping a Config
    // (it cannot call Application::run from a unit test — no compositor).
    // The null-config rejection path of optics_app_run is covered by the C
    // test suite (tests/app/test_host_binding.c), not here.
    let cfg = optics_app::Config::new("noop").unwrap();
    drop(cfg); // no panic on drop
}

#[test]
fn reexported_crates_are_reachable() {
    assert!(!optics_app::gfx::version().is_empty());
    assert!(!optics_app::ui::version().is_empty());
    assert!(!optics_app::window::version().is_empty());
    let mut spring = optics_app::ui::motion::Spring::new(0.0);
    spring.advance(
        100.0,
        optics_app::ui::motion::SpringParams::snappy(),
        1.0 / 60.0,
        false,
    );
    assert!(spring.value > 0.0);
}
