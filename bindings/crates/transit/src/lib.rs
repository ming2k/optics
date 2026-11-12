//! Safe Rust implementation of **transit** — closed-form analytic springs and motion vocabulary (ADR-0077, ADR-0106, ADR-0158).
//!
//! Pure math on caller-owned state: no clock, no timeline, no scheduler. Every
//! primitive takes `dt` explicitly and lets the caller own the animation state.
//! Reduced motion is one flag per advance entry point: when set, every
//! primitive resolves to its end state in a single step (ADR-0029).

#![deny(rust_2018_idioms)]

pub const VERSION_MAJOR: u32 = 0;
pub const VERSION_MINOR: u32 = 0;
pub const VERSION_PATCH: u32 = 79;

pub const VERSION_NUMBER: u32 = (VERSION_MAJOR << 16) | (VERSION_MINOR << 8) | VERSION_PATCH;

/// The largest delta time any primitive integrates over (1/30 s).
pub const DT_MAX: f32 = 1.0 / 30.0;

/// Library version string.
pub fn version() -> &'static str {
    "0.0.79"
}

/// Packed integer version of the library.
pub fn version_number() -> u32 {
    VERSION_NUMBER
}

/// True iff the library satisfies the major.minor.patch floor.
pub fn version_check(major: u32, minor: u32, patch: u32) -> bool {
    if major != VERSION_MAJOR {
        return false;
    }
    if minor > VERSION_MINOR {
        return false;
    }
    if minor == VERSION_MINOR && patch > VERSION_PATCH {
        return false;
    }
    true
}

/// Clamp `dt_seconds` into the integrable range `[0, 1/30]` seconds.
pub fn dt_clamp(dt_seconds: f32) -> f32 {
    if dt_seconds <= 0.0 || dt_seconds.is_nan() {
        0.0
    } else if dt_seconds > DT_MAX {
        DT_MAX
    } else {
        dt_seconds
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
    pub const fn new(stiffness: f32, damping: f32) -> Self {
        Self { stiffness, damping }
    }

    pub fn snappy() -> Self {
        Self {
            stiffness: 480.0,
            damping: 0.90,
        }
    }

    pub fn gentle() -> Self {
        Self {
            stiffness: 220.0,
            damping: 0.80,
        }
    }

    pub fn bouncy() -> Self {
        Self {
            stiffness: 480.0,
            damping: 0.55,
        }
    }
}

impl Spring {
    /// A spring at rest (`velocity == 0`) holding `value`.
    pub fn new(value: f32) -> Self {
        Self {
            value,
            velocity: 0.0,
        }
    }

    /// True when the spring rests on `target` within the given tolerances.
    pub fn settled(&self, target: f32, value_eps: f32, vel_eps: f32) -> bool {
        (self.value - target).abs() <= value_eps && self.velocity.abs() <= vel_eps
    }

    /// One-step resolve (reduced motion, or an animation that must end now).
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
        if target.is_nan() {
            return self.value;
        }
        if reduced_motion {
            return self.snap_to(target);
        }
        let dt = dt_clamp(dt_seconds);
        if dt <= 0.0 {
            return self.value;
        }

        let omega0sq = if params.stiffness > 0.0 {
            params.stiffness
        } else {
            0.0
        };
        let omega0 = omega0sq.sqrt();
        if omega0 <= 0.0 {
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
        if zeta < 1.0 {
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
        if self.value.is_nan() {
            self.value = target;
        }
        if self.velocity.is_nan() {
            self.velocity = 0.0;
        }
        self.value
    }

    /// Advance toward `target` using bounded sub-steps (each `<= max_substep_dt`)
    /// to prevent discrete jumps under irregular frame delivery.
    pub fn advance_substep(
        &mut self,
        target: f32,
        params: SpringParams,
        dt_seconds: f32,
        max_substep_dt: f32,
        reduced_motion: bool,
    ) -> f32 {
        if reduced_motion {
            return self.snap_to(target);
        }
        if target.is_nan() || dt_seconds <= 0.0 {
            return self.value;
        }

        let mut remaining = if dt_seconds > 0.10 { 0.10 } else { dt_seconds };
        let mut step = if max_substep_dt > 0.0 {
            max_substep_dt
        } else {
            1.0 / 60.0
        };
        if step > DT_MAX {
            step = DT_MAX;
        }

        while remaining > 0.0 {
            let sub_dt = if remaining > step { step } else { remaining };
            self.advance(target, params, sub_dt, false);
            remaining -= sub_dt;
        }
        self.value
    }
}

/// Motion-adaptive critically-damped smoother.
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct Smoother {
    pub value: f32,
    pub velocity: f32,
}

impl Smoother {
    pub fn new(initial: f32) -> Self {
        Self {
            value: initial,
            velocity: 0.0,
        }
    }

    /// Advance toward `target`. `reduced_motion` resolves to the target in one
    /// step (velocity cleared).
    pub fn step(
        &mut self,
        target: f32,
        tau_rest: f32,
        motion_scale: f32,
        motion_eps: f32,
        dt: f32,
        reduced_motion: bool,
    ) -> f32 {
        if reduced_motion {
            self.value = target;
            self.velocity = 0.0;
            return target;
        }
        let dt = dt_clamp(dt);
        if dt <= 0.0 {
            return self.value;
        }
        if tau_rest <= 0.0 {
            self.value = target;
            return target;
        }

        let mut tau = tau_rest;
        if motion_scale > 1.0 && (target - self.value).abs() > motion_eps {
            tau = tau_rest * motion_scale;
        }

        let mut sp = Spring {
            value: self.value,
            velocity: self.velocity,
        };
        let p = SpringParams {
            stiffness: 4.0 / (tau * tau),
            damping: 1.0,
        };
        let v = sp.advance(target, p, dt, false);
        self.value = sp.value;
        self.velocity = sp.velocity;
        v
    }
}

/// Schmitt-trigger latch de-jitter primitive.
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
    pub fn new(initial_high: bool) -> Self {
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
        if wants != self.high {
            self.candidate = wants;
            self.dwell += dt;
            if self.dwell >= dwell_secs {
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

/// Move `current` toward `target` by `rate` per second.
/// `reduced_motion` resolves to the target in one step.
pub fn approach(current: f32, target: f32, rate: f32, dt: f32, reduced_motion: bool) -> f32 {
    if target.is_nan() {
        return current;
    }
    if reduced_motion || rate <= 0.0 || rate.is_nan() {
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
pub fn decay(value: f32, rate: f32, dt: f32, reduced_motion: bool) -> f32 {
    if reduced_motion || rate <= 0.0 || rate.is_nan() {
        return 0.0;
    }
    let dt = dt_clamp(dt);
    if dt <= 0.0 {
        return value;
    }
    value * (-rate * dt).exp()
}

pub fn ease_out_cubic(t: f32) -> f32 {
    let inv = 1.0 - t.clamp(0.0, 1.0);
    1.0 - inv * inv * inv
}

pub fn ease_in_cubic(t: f32) -> f32 {
    let c = t.clamp(0.0, 1.0);
    c * c * c
}

pub fn ease_in_out_cubic(t: f32) -> f32 {
    let c = t.clamp(0.0, 1.0);
    if c < 0.5 {
        4.0 * c * c * c
    } else {
        1.0 - (-2.0 * c + 2.0).powi(3) / 2.0
    }
}

pub fn ease_out_back(t: f32) -> f32 {
    let c = t.clamp(0.0, 1.0);
    const C1: f32 = 1.70158;
    const C3: f32 = C1 + 1.0;
    let d = c - 1.0;
    1.0 + C3 * d * d * d + C1 * d * d
}

pub fn smoothstep(t: f32) -> f32 {
    let c = t.clamp(0.0, 1.0);
    c * c * (3.0 - 2.0 * c)
}
