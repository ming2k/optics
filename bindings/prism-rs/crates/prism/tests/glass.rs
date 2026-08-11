//! Compile-and-link coverage for the liquid-glass wrapper: the safe types
//! keep their documented shape, defaults match `PRISM_LIQUID_GLASS_DESC_INIT`,
//! and the prism + flux shared libraries link and run. The filter itself
//! needs a recording Vulkan frame, so GPU coverage stays with the C tests
//! (`meson test`) — mirroring the flux crate, whose effect wrappers are also
//! covered at compile level only.

use prism::{LiquidGlassFocus, LiquidGlassGroup, LiquidGlassParams, LiquidGlassShape};

#[test]
fn params_default_matches_c_init_macro() {
    let p = LiquidGlassParams::default();
    assert_eq!(p.refraction, 8.0);
    assert_eq!(p.chromatic_aberration, 1.25);
    assert_eq!(p.saturation, 1.08);
    assert_eq!(p.brightness, 1.02);
    assert_eq!(p.edge_width, 18.0);
    assert_eq!(p.rim_light, 0.55);
    assert_eq!(p.light_direction, (-0.45, -0.89));
    assert_eq!(p.opacity, 1.0);
    assert_eq!(p.size_reference, 72.0);
    assert_eq!(p.size_scale_min, 0.15);
    assert_eq!(p.tint_strength, 1.0);
    assert_eq!(p.frost_strength, 1.0);
}

#[test]
fn shape_focus_group_are_plain_copy_data() {
    let primary = LiquidGlassShape {
        x: 10.0,
        y: 20.0,
        width: 300.0,
        height: 200.0,
        corner_radius: 16.0,
    };
    let focus = LiquidGlassFocus {
        shape: primary,
        strength: 0.5,
    };
    let group = LiquidGlassGroup {
        primary,
        merged: None,
        blend_radius: 12.0,
        opacity: 1.0,
        shadow_alpha: 0.0,
        shadow_blur: 0.0,
        shadow_offset_y: 0.0,
        tint_color: [255, 255, 255],
        focus: Some(focus),
    };
    let copied = group; // Copy
    assert_eq!(group, copied);
    assert_eq!(copied.focus.expect("focus").strength, 0.5);
    assert_eq!(copied.tint_color, [255, 255, 255]);
}

#[test]
fn error_is_flux_error_and_displays() {
    // prism::Error re-exports flux::Error; Display resolves the code through
    // libflux's flux_result_string — so this also proves the link.
    let err = prism::Error(prism::sys::flux_result::FLUX_ERROR_INVALID_ARGUMENT);
    let text = err.to_string();
    assert!(text.starts_with("flux error:"), "unexpected: {text}");
}
