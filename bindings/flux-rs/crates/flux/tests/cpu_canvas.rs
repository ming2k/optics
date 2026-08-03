//! Headless software-canvas smoke test: exercises the CPU backend end to end
//! from the safe Rust API with no GPU, device, or window.

use flux::{Canvas, GradientStop, rgba};

#[test]
fn cpu_canvas_renders_and_reads_back() {
    let c = Canvas::new_cpu(64, 64, 1.0).expect("create CPU canvas");

    let black = rgba(0, 0, 0, 255);
    let red = rgba(255, 0, 0, 255);

    c.begin_cpu(Some(black)).expect("begin");
    c.fill_rrect(8.0, 8.0, 48.0, 48.0, 12.0, red);
    c.end_checked().expect("end checked CPU pass");

    let (w, h, stride, px) = c.read_pixels().expect("CPU backend exposes pixels");
    assert_eq!((w, h), (64, 64));
    assert_eq!(stride, 64 * 4);

    // Centre is inside the rounded rect → opaque red.
    let center = 32 * stride as usize + 32 * 4;
    assert!(px[center] > 250, "center R = {}", px[center]);
    assert!(px[center + 1] < 5 && px[center + 2] < 5);
    assert!(px[center + 3] > 250, "center A = {}", px[center + 3]);

    // A far corner is outside the rounded corner → cleared black.
    let corner = stride as usize + 4;
    assert!(px[corner] < 5 && px[corner + 3] > 250);
}

#[test]
fn unified_factory_selects_cpu() {
    // The Skia-style path: begin_frame(None, ..) drives a CPU canvas.
    let c = Canvas::new_cpu(32, 32, 1.0).unwrap();
    c.begin_frame(None, Some(rgba(0, 0, 0, 255))).unwrap();
    c.fill_rect(0.0, 0.0, 32.0, 32.0, rgba(0, 0, 255, 255));
    c.end();
    let (_, _, stride, px) = c.read_pixels().unwrap();
    let p = 16 * stride as usize + 16 * 4;
    assert!(px[p + 2] > 250 && px[p] < 5); // blue
}

#[test]
fn rgba_premultiplies_translucent_colours_for_src_over() {
    let c = Canvas::new_cpu(1, 1, 1.0).unwrap();
    c.begin_cpu(Some(rgba(0, 0, 0, 255))).unwrap();
    c.fill_rect(0.0, 0.0, 1.0, 1.0, rgba(255, 255, 255, 32));
    c.end();
    let (_, _, _, pixels) = c.read_pixels().expect("CPU readback");
    assert!(
        (30..=34).contains(&pixels[0]),
        "translucent white over black should remain near 32, got {}",
        pixels[0]
    );
    assert_eq!(pixels[0], pixels[1]);
    assert_eq!(pixels[1], pixels[2]);
    assert_eq!(pixels[3], 255);
}

#[test]
fn safe_radial_gradient_reaches_canvas_backend() {
    let c = Canvas::new_cpu(32, 32, 1.0).unwrap();
    c.begin_cpu(Some(rgba(0, 0, 0, 255))).unwrap();
    c.fill_rect_radial_gradient(
        (0.0, 0.0, 32.0, 32.0),
        (16.0, 16.0),
        16.0,
        &[
            GradientStop::new(0.0, rgba(255, 64, 32, 255)),
            GradientStop::new(1.0, rgba(255, 64, 32, 0)),
        ],
    );
    c.end();
    let (_, _, stride, pixels) = c.read_pixels().expect("CPU readback");
    let center = 16 * stride as usize + 16 * 4;
    let corner = stride as usize + 4;
    assert!(pixels[center] > 200, "gradient center should be red");
    assert!(pixels[corner] < 20, "gradient corner should fade to black");
}

#[test]
fn radial_gradient_follows_canvas_transform() {
    // Under a 2x canvas transform the gradient geometry must be evaluated in
    // framebuffer pixel space: centre (32,32) radius 16 becomes centre (64,64)
    // radius 32. Regression test for build_push copying gradient parameters
    // without applying the canvas transform.
    let c = Canvas::new_cpu(128, 128, 1.0).unwrap();
    c.begin_cpu(Some(rgba(0, 0, 0, 255))).unwrap();
    c.save();
    c.scale(2.0, 2.0);
    c.fill_rect_radial_gradient(
        (0.0, 0.0, 64.0, 64.0),
        (32.0, 32.0),
        16.0,
        &[
            GradientStop::new(0.0, rgba(255, 64, 32, 255)),
            GradientStop::new(1.0, rgba(255, 64, 32, 0)),
        ],
    );
    c.restore();
    c.end();
    let (_, _, stride, pixels) = c.read_pixels().expect("CPU readback");
    let scaled_center = 64 * stride as usize + 64 * 4;
    let unscaled_center = 32 * stride as usize + 32 * 4;
    assert!(
        pixels[scaled_center] > 200,
        "gradient should centre at (64,64) after scaling, R = {}",
        pixels[scaled_center]
    );
    assert!(
        pixels[unscaled_center] < 20,
        "unscaled position (32,32) should fade out, R = {}",
        pixels[unscaled_center]
    );
}

#[test]
fn linear_gradient_follows_canvas_transform() {
    // from (0,0) to (64,0) under a 2x transform spans device x 0..=128.
    let c = Canvas::new_cpu(128, 128, 1.0).unwrap();
    c.begin_cpu(Some(rgba(0, 0, 0, 255))).unwrap();
    c.save();
    c.scale(2.0, 2.0);
    c.fill_rect_linear_gradient(
        (0.0, 0.0, 64.0, 64.0),
        (0.0, 0.0),
        (64.0, 0.0),
        &[
            GradientStop::new(0.0, rgba(255, 0, 0, 255)),
            GradientStop::new(1.0, rgba(255, 0, 0, 0)),
        ],
    );
    c.restore();
    c.end();
    let (_, _, stride, pixels) = c.read_pixels().expect("CPU readback");
    let near_start = 64 * stride as usize + 8 * 4;
    let mid = 64 * stride as usize + 64 * 4;
    assert!(
        pixels[near_start] > 200,
        "device x=8 should still be near full red, R = {}",
        pixels[near_start]
    );
    assert!(
        (90..=170).contains(&pixels[mid]),
        "device x=64 should sit mid-gradient, R = {}",
        pixels[mid]
    );
}
