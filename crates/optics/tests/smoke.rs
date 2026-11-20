#[test]
fn test_facade_motion() {
    #[cfg(feature = "motion")]
    {
        use optics::motion::{Spring, SpringParams};
        let mut spring = Spring::new(0.0);
        let params = SpringParams::snappy();
        spring.advance(100.0, params, 1.0 / 60.0, false);
        assert!(spring.value > 0.0);
    }
}

#[test]
fn test_facade_gfx() {
    #[cfg(feature = "gfx")]
    {
        let v = optics::gfx::version();
        assert!(!v.is_empty());
    }
}

#[test]
fn test_facade_ui() {
    #[cfg(feature = "ui")]
    {
        let v = optics::ui::version();
        assert!(!v.is_empty());
    }
}

#[test]
fn test_facade_window() {
    #[cfg(feature = "window")]
    {
        let v = optics::window::version();
        assert!(!v.is_empty());
    }
}
