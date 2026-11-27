//! Closed-form UI motion vocabulary (ADR-0177).
//!
//! Pure Safe Rust mirror of `<optics/ui/motion.h>` with zero FFI overhead:
//! no clock, no timeline, no scheduler, no allocation, and no global state.
//! Every primitive takes `dt` explicitly and lets the caller own the state.
//! Reduced motion is one flag per advance call: when set, every primitive
//! resolves to its target end state in a single step.

/// The largest delta time any motion primitive integrates over (`1/30` s).
pub const DT_MAX: f32 = 1.0 / 30.0;

/// Maximum sub-steps permitted in a single [`Spring::advance_substep`] call.
pub const SUBSTEP_MAX_COUNT: u32 = 64;

/// Clamp `dt_seconds` into the integrable range `[0, 1/30]` seconds.
#[must_use]
#[inline]
pub fn dt_clamp(dt_seconds: f32) -> f32 {
    if dt_seconds <= 0.0 || dt_seconds.is_nan() {
        0.0
    } else if dt_seconds > DT_MAX {
        DT_MAX
    } else {
        dt_seconds
    }
}

#[inline]
fn clamp01(t: f32) -> f32 {
    if t <= 0.0 || t.is_nan() {
        0.0
    } else if t >= 1.0 {
        1.0
    } else {
        t
    }
}

/// Closed-form damped harmonic oscillator spring.
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct Spring {
    pub value: f32,
    pub velocity: f32,
}

/// Tuning parameters for [`Spring`].
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct SpringParams {
    /// ω₀² — stiffness, in (rad/s)².
    pub stiffness: f32,
    /// ζ — damping ratio: `1.0` critically damped, `< 1.0` slight overshoot.
    pub damping: f32,
}

impl SpringParams {
    #[must_use]
    #[inline]
    pub const fn new(stiffness: f32, damping: f32) -> Self {
        Self { stiffness, damping }
    }

    #[must_use]
    #[inline]
    pub const fn snappy() -> Self {
        Self {
            stiffness: 480.0,
            damping: 0.90,
        }
    }

    #[must_use]
    #[inline]
    pub const fn gentle() -> Self {
        Self {
            stiffness: 220.0,
            damping: 0.80,
        }
    }

    #[must_use]
    #[inline]
    pub const fn bouncy() -> Self {
        Self {
            stiffness: 480.0,
            damping: 0.55,
        }
    }
}

impl Spring {
    /// A spring at rest (`velocity == 0`) holding `value`.
    #[must_use]
    #[inline]
    pub const fn new(value: f32) -> Self {
        Self {
            value,
            velocity: 0.0,
        }
    }

    /// True when the spring rests on `target` within the given tolerances.
    #[must_use]
    #[inline]
    pub fn settled(&self, target: f32, value_eps: f32, vel_eps: f32) -> bool {
        (self.value - target).abs() <= value_eps && self.velocity.abs() <= vel_eps
    }

    /// One-step resolve (reduced motion, or an animation that must end now).
    #[inline]
    pub fn snap_to(&mut self, target: f32) -> f32 {
        self.value = target;
        self.velocity = 0.0;
        target
    }

    /// Advance toward `target` by clamped `dt_seconds`; returns the new value.
    /// `reduced_motion` resolves to the target in one step.
    pub fn advance(
        &mut self,
        target: f32,
        params: SpringParams,
        dt_seconds: f32,
        reduced_motion: bool,
    ) -> f32 {
        if !target.is_finite() {
            return self.value;
        }
        if reduced_motion {
            return self.snap_to(target);
        }
        if !self.value.is_finite() || !self.velocity.is_finite() {
            return self.snap_to(target);
        }
        let dt = dt_clamp(dt_seconds);
        if dt <= 0.0 {
            return self.value;
        }

        if !params.stiffness.is_finite() || params.stiffness <= 0.0 {
            return self.snap_to(target);
        }
        let omega0 = params.stiffness.sqrt();
        if !omega0.is_finite() || omega0 <= 0.0 {
            return self.snap_to(target);
        }
        let mut zeta = params.damping;
        if zeta < 0.0 || zeta.is_nan() {
            zeta = 1.0;
        }
        if zeta > 1.0 {
            zeta = 1.0;
        }

        let x = self.value - target;
        if zeta < 1.0 - 1e-4 {
            let decay_rate = zeta * omega0;
            let omega_d = omega0 * (1.0 - zeta * zeta).sqrt();
            let decay = (-decay_rate * dt).exp();
            let sin_ = (omega_d * dt).sin();
            let cos_ = (omega_d * dt).cos();
            let vterm = (self.velocity + decay_rate * x) / omega_d;
            self.value = target + decay * (x * cos_ + vterm * sin_);
            self.velocity = decay
                * (self.velocity * cos_
                    - (decay_rate * self.velocity + omega0 * omega0 * x) / omega_d * sin_);
        } else {
            let decay = (-omega0 * dt).exp();
            let vterm = self.velocity + omega0 * x;
            self.value = target + decay * (x + vterm * dt);
            self.velocity = decay * (self.velocity - omega0 * vterm * dt);
        }
        if !self.value.is_finite() {
            self.value = target;
        }
        if !self.velocity.is_finite() {
            self.velocity = 0.0;
        }
        self.value
    }

    /// Advance toward `target` using bounded sub-steps (each `<= max_substep_dt`)
    /// to prevent discrete jumps under irregular frame delivery.
    /// Step count is capped at [`SUBSTEP_MAX_COUNT`] to guarantee O(1) termination.
    pub fn advance_substep(
        &mut self,
        target: f32,
        params: SpringParams,
        dt_seconds: f32,
        max_substep_dt: f32,
        reduced_motion: bool,
    ) -> f32 {
        if !target.is_finite() {
            return self.value;
        }
        if reduced_motion {
            return self.snap_to(target);
        }
        if dt_seconds <= 0.0 || dt_seconds.is_nan() {
            return self.value;
        }

        let total = dt_seconds.min(0.10);
        let mut step = if max_substep_dt > 0.0 && max_substep_dt.is_finite() {
            max_substep_dt
        } else {
            1.0 / 60.0
        };
        if step > DT_MAX {
            step = DT_MAX;
        }
        let min_step = total / SUBSTEP_MAX_COUNT as f32;
        if step < min_step {
            step = min_step;
        }

        let steps = ((total / step).ceil() as u32).clamp(1, SUBSTEP_MAX_COUNT);
        let sub_dt = total / steps as f32;
        for _ in 0..steps {
            self.advance(target, params, sub_dt, false);
        }
        self.value
    }
}

/// Schmitt-trigger latch de-jitter primitive for binary UI state.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Hysteresis {
    pub high: bool,
    pub dwell: f32,
    pub candidate: bool,
}

impl Default for Hysteresis {
    fn default() -> Self {
        Self::new(false)
    }
}

impl Hysteresis {
    #[must_use]
    #[inline]
    pub const fn new(initial_high: bool) -> Self {
        Self {
            high: initial_high,
            dwell: 0.0,
            candidate: initial_high,
        }
    }

    /// Advance the latch. `reduced_motion` latches to the input's request in
    /// one step, bypassing the dwell timer.
    #[allow(clippy::too_many_arguments)]
    pub fn step(
        &mut self,
        input_high: bool,
        low_th: f32,
        high_th: f32,
        measurement: f32,
        dwell_secs: f32,
        dt: f32,
        reduced_motion: bool,
    ) -> bool {
        let wants = if low_th < high_th && !measurement.is_nan() {
            if self.high {
                measurement >= low_th
            } else {
                measurement > high_th
            }
        } else {
            input_high
        };

        if reduced_motion {
            self.high = wants;
            self.candidate = wants;
            self.dwell = 0.0;
            return self.high;
        }

        let dt = dt_clamp(dt);
        let dwell_req = if dwell_secs > 0.0 { dwell_secs } else { 0.0 };
        if wants != self.high {
            self.candidate = wants;
            self.dwell += dt;
            if self.dwell >= dwell_req {
                self.high = wants;
                self.dwell = 0.0;
            }
        } else {
            self.dwell = 0.0;
            self.candidate = wants;
        }
        self.high
    }
}

/// Move `current` toward `target` by `rate` per second (frame-rate independent).
/// `reduced_motion` resolves to the target in one step.
#[must_use]
#[inline]
pub fn approach(current: f32, target: f32, rate: f32, dt: f32, reduced_motion: bool) -> f32 {
    if !target.is_finite() {
        return current;
    }
    if reduced_motion || !current.is_finite() {
        return target;
    }
    if rate <= 0.0 || !rate.is_finite() {
        return target;
    }
    let dt = dt_clamp(dt);
    if dt <= 0.0 {
        return current;
    }
    if (target - current).abs() < 1e-6 {
        return target;
    }
    current + (target - current) * (1.0 - (-rate * dt).exp())
}

/// Exponential decay toward zero (opacity tails, trailing values).
/// `reduced_motion` resolves to zero in one step.
#[must_use]
#[inline]
pub fn decay(value: f32, rate: f32, dt: f32, reduced_motion: bool) -> f32 {
    if reduced_motion || !value.is_finite() {
        return 0.0;
    }
    if rate <= 0.0 || !rate.is_finite() {
        return 0.0;
    }
    let dt = dt_clamp(dt);
    if dt <= 0.0 {
        return value;
    }
    value * (-rate * dt).exp()
}

/// Cubic ease-out over normalized `t` in `[0, 1]`.
#[must_use]
#[inline]
pub fn ease_out_cubic(t: f32) -> f32 {
    let inv = 1.0 - clamp01(t);
    1.0 - inv * inv * inv
}

/// Cubic ease-in over normalized `t` in `[0, 1]`.
#[must_use]
#[inline]
pub fn ease_in_cubic(t: f32) -> f32 {
    let c = clamp01(t);
    c * c * c
}

/// Symmetric cubic ease-in-out over normalized `t` in `[0, 1]`.
#[must_use]
#[inline]
pub fn ease_in_out_cubic(t: f32) -> f32 {
    let c = clamp01(t);
    if c < 0.5 {
        4.0 * c * c * c
    } else {
        let k = -2.0 * c + 2.0;
        1.0 - (k * k * k) * 0.5
    }
}

/// Slight overshoot ease-out over normalized `t` in `[0, 1]`.
#[must_use]
#[inline]
pub fn ease_out_back(t: f32) -> f32 {
    let c = clamp01(t);
    const C1: f32 = 1.70158;
    const C3: f32 = C1 + 1.0;
    let d = c - 1.0;
    1.0 + C3 * d * d * d + C1 * d * d
}

/// Hermite smoothstep over normalized `t` in `[0, 1]`.
#[must_use]
#[inline]
pub fn smoothstep(t: f32) -> f32 {
    let c = clamp01(t);
    c * c * (3.0 - 2.0 * c)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn spring_converges_and_energy_never_increases() {
        let mut spring = Spring::new(0.0);
        let params = SpringParams::snappy();
        for _ in 0..60 {
            spring.advance(100.0, params, 1.0 / 60.0, false);
        }
        assert!(spring.settled(100.0, 0.1, 0.1));

        let mut e_spring = Spring {
            value: 0.0,
            velocity: 40.0,
        };
        let gentle = SpringParams::gentle();
        let mut prev_e = f32::INFINITY;
        for _ in 0..200 {
            e_spring.advance(0.0, gentle, 1.0 / 60.0, false);
            let e = 0.5 * gentle.stiffness * e_spring.value * e_spring.value
                + 0.5 * e_spring.velocity * e_spring.velocity;
            assert!(e <= prev_e + 1e-2);
            prev_e = e;
        }
    }

    #[test]
    fn spring_advance_substep_is_path_independent_and_bounded() {
        let params = SpringParams::snappy();
        let mut s1 = Spring::new(0.0);
        let mut s2 = Spring::new(0.0);

        let v1 = s1.advance_substep(100.0, params, 0.050, 0.016, false);
        s2.advance(100.0, params, 0.016, false);
        s2.advance(100.0, params, 0.016, false);
        s2.advance(100.0, params, 0.016, false);
        s2.advance(100.0, params, 0.002, false);
        assert!((v1 - s2.value).abs() < 1e-4);
        assert!((s1.velocity - s2.velocity).abs() < 1e-3);

        // Adversarial tiny max_substep_dt must terminate in O(1) steps and match the analytic solution.
        let mut s_tiny = Spring::new(0.0);
        let v_tiny = s_tiny.advance_substep(100.0, params, 0.050, 1e-10, false);
        assert!(v_tiny.is_finite());
        assert!((v_tiny - s2.value).abs() < 1e-3);

        let mut s_red = Spring::new(0.0);
        assert_eq!(
            s_red.advance_substep(50.0, params, 0.050, 1.0 / 60.0, true),
            50.0
        );
        assert_eq!(s_red.velocity, 0.0);
    }

    #[test]
    fn spring_near_critical_damping_and_non_finite_inputs_are_safe() {
        for zeta in [0.9999_f32, 0.9999999, 1.0, 1.5, f32::NAN] {
            let mut s = Spring::new(0.0);
            let p = SpringParams::new(400.0, zeta);
            for _ in 0..120 {
                let v = s.advance(100.0, p, 1.0 / 60.0, false);
                assert!(v.is_finite());
                assert!(s.velocity.is_finite());
                assert!((-1e-3..=100.0 + 1e-2).contains(&v));
            }
            assert!(s.settled(100.0, 1e-2, 1e-1));
        }

        let mut s_inf = Spring {
            value: f32::INFINITY,
            velocity: f32::NEG_INFINITY,
        };
        assert_eq!(
            s_inf.advance(10.0, SpringParams::snappy(), 1.0 / 60.0, false),
            10.0
        );
        assert_eq!(s_inf.velocity, 0.0);
    }

    #[test]
    fn reduced_motion_resolves_every_primitive_in_one_step() {
        let mut spring = Spring::new(0.0);
        assert_eq!(
            spring.advance(1.0, SpringParams::gentle(), 1.0 / 60.0, true),
            1.0
        );
        assert_eq!(approach(0.0, 1.0, 1.0, 1.0 / 60.0, true), 1.0);
        assert_eq!(decay(1.0, 1.0, 1.0 / 60.0, true), 0.0);

        let mut h = Hysteresis::new(false);
        assert!(h.step(true, 0.0, 0.0, 0.0, 10.0, 0.0, true));
        assert!(h.high);
        assert_eq!(h.dwell, 0.0);
    }

    #[test]
    fn approach_is_frame_rate_independent() {
        let one_60 = approach(0.0, 1.0, 14.0, 1.0 / 60.0, false);
        let mut two_120 = approach(0.0, 1.0, 14.0, 1.0 / 120.0, false);
        two_120 = approach(two_120, 1.0, 14.0, 1.0 / 120.0, false);
        assert!((one_60 - two_120).abs() < 1e-6);
        assert_eq!(approach(f32::NAN, 1.0, 14.0, 1.0 / 60.0, false), 1.0);
        assert_eq!(decay(f32::INFINITY, 14.0, 1.0 / 60.0, false), 0.0);
    }

    #[test]
    fn easing_nan_and_hysteresis_nan_dwell_are_safe() {
        assert_eq!(ease_out_cubic(f32::NAN), 0.0);
        assert_eq!(ease_in_cubic(f32::NAN), 0.0);
        assert_eq!(ease_in_out_cubic(f32::NAN), 0.0);
        assert_eq!(ease_out_back(f32::NAN), 0.0);
        assert_eq!(smoothstep(f32::NAN), 0.0);

        assert_eq!(smoothstep(0.0), 0.0);
        assert_eq!(smoothstep(1.0), 1.0);
        assert!((smoothstep(0.5) - 0.5).abs() < 1e-6);
        assert!((smoothstep(0.2) + smoothstep(0.8) - 1.0).abs() < 1e-6);
        assert_eq!(smoothstep(-0.5), 0.0);
        assert_eq!(smoothstep(1.5), 1.0);
        assert_eq!(ease_in_out_cubic(0.5), 0.5);

        let mut h = Hysteresis::new(false);
        assert!(!h.step(true, 0.3, 0.7, 0.5, 0.2, 0.016, false));
        // NaN dwell must not lock the latch forever.
        assert!(h.step(true, 0.0, 0.0, 0.0, f32::NAN, 0.016, false));
    }

    #[test]
    fn motion_types_are_send_and_sync() {
        fn assert_send<T: Send>() {}
        fn assert_sync<T: Sync>() {}

        assert_send::<Spring>();
        assert_sync::<Spring>();
        assert_send::<SpringParams>();
        assert_sync::<SpringParams>();
        assert_send::<Hysteresis>();
        assert_sync::<Hysteresis>();
    }
}
