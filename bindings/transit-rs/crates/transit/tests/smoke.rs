use transit::{Spring, SpringParams, Smoother, Hysteresis};

#[test]
fn test_version_smoke() {
    let s = transit::version();
    assert!(!s.is_empty());
    assert!(transit::version_number() > 0);
    assert!(transit::version_check(0, 0, 50));
}

#[test]
fn test_spring_dynamics() {
    let mut spring = Spring::new(0.0);
    let params = SpringParams::snappy();
    let target = 100.0;
    for _ in 0..60 {
        spring.advance(target, params, 1.0 / 60.0, false);
    }
    assert!(spring.settled(target, 0.1, 0.1));
    assert!((spring.value - target).abs() < 0.1);
}

#[test]
fn test_smoother_and_hysteresis() {
    let mut smoother = Smoother::new(0.0);
    let v = smoother.step(10.0, 0.1, 2.0, 0.1, 0.016);
    assert!(v > 0.0);

    let mut h = Hysteresis::new(false);
    assert!(!h.step(true, 0.3, 0.7, 0.5, 0.2, 0.016));
}

#[test]
fn test_thread_safety() {
    fn assert_send<T: Send>() {}
    fn assert_sync<T: Sync>() {}

    assert_send::<Spring>();
    assert_sync::<Spring>();
    assert_send::<SpringParams>();
    assert_sync::<SpringParams>();
    assert_send::<Smoother>();
    assert_sync::<Smoother>();
    assert_send::<Hysteresis>();
    assert_sync::<Hysteresis>();
}
