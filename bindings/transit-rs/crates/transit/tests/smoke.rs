use transit::{Hysteresis, Smoother, Spring, SpringParams};

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
fn test_reduced_motion_resolves_every_primitive_in_one_step() {
    // The ADR-0029 rule is uniform across the vocabulary: one flag, one step.
    let mut spring = Spring::new(0.0);
    assert_eq!(spring.advance(1.0, SpringParams::gentle(), 1.0 / 60.0, true), 1.0);

    assert_eq!(transit::approach(0.0, 1.0, 1.0, 1.0 / 60.0, true), 1.0);
    assert_eq!(transit::decay(1.0, 1.0, 1.0 / 60.0, true), 0.0);

    let mut smoother = Smoother::new(0.0);
    assert_eq!(smoother.step(1.0, 0.1, 2.0, 0.1, 1.0 / 60.0, true), 1.0);
    assert_eq!(smoother.value, 1.0);
    assert_eq!(smoother.velocity, 0.0);

    // A long dwell must not delay the latch under reduced motion.
    let mut h = Hysteresis::new(false);
    assert!(h.step(true, 0.0, 0.0, 0.0, 10.0, 0.0, true));
    assert!(h.high);
}

#[test]
fn test_smoother_and_hysteresis() {
    let mut smoother = Smoother::new(0.0);
    let v = smoother.step(10.0, 0.1, 2.0, 0.1, 0.016, false);
    assert!(v > 0.0);

    let mut h = Hysteresis::new(false);
    assert!(!h.step(true, 0.3, 0.7, 0.5, 0.2, 0.016, false));
}

#[test]
fn test_smoothstep() {
    assert_eq!(transit::smoothstep(0.0), 0.0);
    assert_eq!(transit::smoothstep(1.0), 1.0);
    assert!((transit::smoothstep(0.5) - 0.5).abs() < 1e-6);
    assert!((transit::smoothstep(0.2) + transit::smoothstep(0.8) - 1.0).abs() < 1e-6);
    assert_eq!(transit::smoothstep(-0.5), 0.0);
    assert_eq!(transit::smoothstep(1.5), 1.0);
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
