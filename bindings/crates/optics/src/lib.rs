//! Optics — high-performance modern graphics and UI engine.
//!
//! # Modular Feature Layers
//! - `canvas`: 2D vector canvas, Vulkan 1.3 RHI, 3D draw core, compute, CPU fallback.
//! - `typography`: HarfBuzz text shaping, layout metrics, dynamic font atlas.
//! - `ui`: Immediate-mode UI engine, Flex/Grid layout, retained store state machine.
//! - `platform`: Native OS windowing (Wayland, Win32, Cocoa), display pacing, event pump.
//! - `materials`: Dielectric Liquid Glass, Frosted Glass, Acrylic, Mica BRDFs.
//! - `motion`: Provable motion physics, closed-form analytic springs, and easing.
//! - `reticle`: SVG vector document static-art compiler.
//! - `scene`: 3D scene graph, camera hierarchy, and spatial transforms.

#[cfg(feature = "canvas")]
pub use flux as canvas;

#[cfg(feature = "typography")]
pub use glyph as typography;

#[cfg(feature = "ui")]
pub use lens as ui;

#[cfg(feature = "platform")]
pub use iris as platform;

#[cfg(feature = "materials")]
pub use prism as materials;

#[cfg(feature = "reticle")]
pub use reticle;

#[cfg(feature = "scene")]
pub use vista as scene;

#[cfg(feature = "motion")]
pub use transit as motion;
