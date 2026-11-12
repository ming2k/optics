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
fn test_facade_canvas() {
    #[cfg(feature = "canvas")]
    {
        let v = optics::canvas::version();
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
fn test_facade_platform() {
    #[cfg(feature = "platform")]
    {
        let v = optics::platform::version();
        assert!(!v.is_empty());
    }
}
