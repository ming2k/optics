//! Safe Rust bindings for **transit** — closed-form analytic springs and motion vocabulary (ADR-0077, ADR-0106).

#![deny(rust_2018_idioms)]

pub use transit_sys as sys;

/// Library version string from the linked library.
pub fn version() -> &'static str {
    unsafe { std::ffi::CStr::from_ptr(sys::transit_version_string()) }
        .to_str()
        .unwrap_or("<invalid>")
}

/// Packed integer version of the linked library.
pub fn version_number() -> u32 {
    unsafe { sys::transit_version_number() }
}

/// True iff the linked library satisfies the major.minor.patch floor.
pub fn version_check(major: u32, minor: u32, patch: u32) -> bool {
    unsafe { sys::transit_version_check(major as i32, minor as i32, patch as i32) }
}

/// Closed-form damped harmonic oscillator spring.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Spring {
    pub value: f32,
    pub velocity: f32,
}

impl Spring {
    pub fn new(value: f32) -> Self {
        Self { value, velocity: 0.0 }
    }

    pub fn settled(&self, target: f32, value_eps: f32, vel_eps: f32) -> bool {
        let raw = sys::transit_spring {
            value: self.value,
            velocity: self.velocity,
        };
        unsafe { sys::transit_spring_settled(&raw, target, value_eps, vel_eps) }
    }

    pub fn advance(&mut self, target: f32, params: SpringParams, dt_seconds: f32, reduced_motion: bool) -> f32 {
        let mut raw = sys::transit_spring {
            value: self.value,
            velocity: self.velocity,
        };
        let res = unsafe {
            sys::transit_spring_advance(
                &mut raw,
                target,
                params.raw(),
                dt_seconds,
                reduced_motion,
            )
        };
        self.value = raw.value;
        self.velocity = raw.velocity;
        res
    }

    pub fn snap_to(&mut self, target: f32) -> f32 {
        let mut raw = sys::transit_spring {
            value: self.value,
            velocity: self.velocity,
        };
        let res = unsafe { sys::transit_spring_snap_to(&mut raw, target) };
        self.value = raw.value;
        self.velocity = raw.velocity;
        res
    }
}

/// Tuning parameters for [`Spring`].
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct SpringParams {
    pub stiffness: f32,
    pub damping: f32,
}

impl SpringParams {
    pub fn snappy() -> Self {
        let p = unsafe { sys::transit_spring_snappy() };
        Self { stiffness: p.stiffness, damping: p.damping }
    }

    pub fn gentle() -> Self {
        let p = unsafe { sys::transit_spring_gentle() };
        Self { stiffness: p.stiffness, damping: p.damping }
    }

    pub fn bouncy() -> Self {
        let p = unsafe { sys::transit_spring_bouncy() };
        Self { stiffness: p.stiffness, damping: p.damping }
    }

    pub(crate) fn raw(self) -> sys::transit_spring_params {
        sys::transit_spring_params {
            stiffness: self.stiffness,
            damping: self.damping,
        }
    }
}

/// Motion-adaptive critically-damped smoother.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Smoother {
    pub value: f32,
    pub velocity: f32,
}

impl Smoother {
    pub fn new(initial: f32) -> Self {
        Self { value: initial, velocity: 0.0 }
    }

    pub fn step(&mut self, target: f32, tau_rest: f32, motion_scale: f32, motion_eps: f32, dt: f32) -> f32 {
        let mut raw = sys::transit_smoother {
            value: self.value,
            velocity: self.velocity,
        };
        let res = unsafe {
            sys::transit_smoother_step(
                &mut raw,
                target,
                tau_rest,
                motion_scale,
                motion_eps,
                dt,
            )
        };
        self.value = raw.value;
        self.velocity = raw.velocity;
        res
    }
}

/// Schmitt-trigger latch de-jitter primitive.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Hysteresis {
    pub high: bool,
    pub dwell: f32,
    pub candidate: bool,
}

impl Hysteresis {
    pub fn new(initial_high: bool) -> Self {
        Self {
            high: initial_high,
            dwell: 0.0,
            candidate: initial_high,
        }
    }

    pub fn step(&mut self, input_high: bool, low_th: f32, high_th: f32, measurement: f32, dwell_secs: f32, dt: f32) -> bool {
        let mut raw = sys::transit_hysteresis {
            high: self.high,
            dwell: self.dwell,
            candidate: self.candidate,
        };
        let res = unsafe {
            sys::transit_hysteresis_step(
                &mut raw,
                input_high,
                low_th,
                high_th,
                measurement,
                dwell_secs,
                dt,
            )
        };
        self.high = raw.high;
        self.dwell = raw.dwell;
        self.candidate = raw.candidate;
        res
    }
}

pub fn approach(current: f32, target: f32, rate: f32, dt: f32) -> f32 {
    unsafe { sys::transit_approach(current, target, rate, dt) }
}

pub fn decay(value: f32, rate: f32, dt: f32) -> f32 {
    unsafe { sys::transit_decay(value, rate, dt) }
}

pub fn ease_out_cubic(t: f32) -> f32 {
    unsafe { sys::transit_ease_out_cubic(t) }
}

pub fn ease_in_cubic(t: f32) -> f32 {
    unsafe { sys::transit_ease_in_cubic(t) }
}

pub fn ease_in_out_cubic(t: f32) -> f32 {
    unsafe { sys::transit_ease_in_out_cubic(t) }
}

pub fn ease_out_back(t: f32) -> f32 {
    unsafe { sys::transit_ease_out_back(t) }
}
