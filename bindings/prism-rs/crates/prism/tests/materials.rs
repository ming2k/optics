//! Tests for frosted glass and acrylic materials in prism-rs.

use prism::{
    AcrylicGroup, AcrylicParams, AcrylicShape, FrostedGroup, FrostedParams, FrostedShape,
};

#[test]
fn frosted_params_default() {
    let p = FrostedParams::default();
    assert_eq!(p.opacity, 1.0);
    assert_eq!(p.saturation, 1.25);
    assert_eq!(p.tint_strength, 0.15);
    assert_eq!(p.noise_intensity, 0.015);
}

#[test]
fn frosted_group_mapping() {
    let group = FrostedGroup {
        shape: FrostedShape {
            x: 10.0,
            y: 20.0,
            width: 100.0,
            height: 50.0,
            corner_radius: 8.0,
        },
        opacity: 0.9,
        tint_color: [255, 128, 64],
        tint_strength: Some(0.3),
        saturation: None,
        shadow_alpha: 0.25,
        shadow_blur: 10.0,
        shadow_offset_y: 4.0,
        noise_intensity: Some(0.02),
    };

    let raw = group.as_raw();
    assert_eq!(raw.shape.bounds.x, 10.0);
    assert_eq!(raw.shape.bounds.y, 20.0);
    assert_eq!(raw.shape.bounds.w, 100.0);
    assert_eq!(raw.shape.bounds.h, 50.0);
    assert_eq!(raw.shape.corner_radius, 8.0);
    assert_eq!(raw.opacity, 0.9);
    assert_eq!(raw.tint_color, 0xFF8040);
    assert_eq!(raw.tint_strength, 0.3);
    assert_eq!(raw.saturation, -1.0); // Sentinel
    assert_eq!(raw.noise_intensity, 0.02);
}

#[test]
fn acrylic_params_default() {
    let p = AcrylicParams::default();
    assert_eq!(p.opacity, 1.0);
    assert_eq!(p.tint_strength, 0.25);
    assert_eq!(p.luminance_plate, 0.5);
    assert_eq!(p.noise_intensity, 0.02);
    assert_eq!(p.border_width, 1.0);
    assert_eq!(p.border_alpha, 0.12);
}

#[test]
fn acrylic_group_mapping() {
    let group = AcrylicGroup {
        shape: AcrylicShape {
            x: 0.0,
            y: 0.0,
            width: 200.0,
            height: 150.0,
            corner_radius: 12.0,
        },
        opacity: 1.0,
        tint_color: [255, 255, 255],
        tint_strength: None,
        luminance_plate: Some(0.8),
        noise_intensity: None,
        border_width: Some(1.5),
        border_alpha: Some(0.2),
        shadow_alpha: 0.4,
        shadow_blur: 16.0,
        shadow_offset_y: 6.0,
    };

    let raw = group.as_raw();
    assert_eq!(raw.shape.bounds.w, 200.0);
    assert_eq!(raw.shape.bounds.h, 150.0);
    assert_eq!(raw.luminance_plate, 0.8);
    assert_eq!(raw.tint_strength, -1.0); // Sentinel
    assert_eq!(raw.border_width, 1.5);
    assert_eq!(raw.border_alpha, 0.2);
}

#[test]
fn backdrop_frost_maps_rect_and_opacity() {
    let frost = prism::BackdropFrost {
        x: 4.0,
        y: 8.0,
        width: 320.0,
        height: 200.0,
        corner_radius: 18.0,
        opacity: 0.85,
        tint_color: [0x10, 0x22, 0x3A],
        tint_strength: 0.6,
    };
    let raw = frost.as_raw();
    assert_eq!((raw.bounds.x, raw.bounds.y, raw.bounds.w, raw.bounds.h), (4.0, 8.0, 320.0, 200.0));
    assert_eq!(raw.corner_radius, 18.0);
    assert_eq!(raw.opacity, 0.85);
    assert_eq!(raw.tint_color, 0x0010_223A);
    assert_eq!(raw.tint_strength, 0.6);
}
