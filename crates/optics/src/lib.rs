//! Optics — pure C23 2D graphics and UI stack for Rust.
//!
//! # Canonical Semantic Modules (ADR-0170)
//! - `gfx`: 2D vector canvas, Vulkan 1.3 RHI, compute, image effects, CPU fallback.
//! - `text`: HarfBuzz text shaping, layout metrics, dynamic font atlas.
//! - `motion`: Closed-form analytic springs and parametric easing.
//! - `window`: Native OS windowing (Wayland, Win32, Cocoa), display pacing, event pump.
//! - `ui`: Headless immediate-mode UI engine, Flex/Grid layout, retained store.
//! - `app`: Application shell binding `ui` to `window`.

#[cfg(feature = "gfx")]
pub use optics_gfx as gfx;

#[cfg(feature = "text")]
pub use optics_text as text;

#[cfg(feature = "motion")]
pub use optics_motion as motion;

#[cfg(feature = "window")]
pub use optics_window as window;

#[cfg(feature = "ui")]
pub use optics_ui as ui;

#[cfg(feature = "app")]
pub use optics_app as app;
