//! Headless software-canvas smoke test: exercises the CPU backend end to end
//! from the safe Rust API with no GPU, device, or window.

use flux::{rgba, Canvas};

#[test]
fn cpu_canvas_renders_and_reads_back() {
    let c = Canvas::new_cpu(64, 64, 1.0).expect("create CPU canvas");

    let black = rgba(0, 0, 0, 255);
    let red = rgba(255, 0, 0, 255);

    c.begin_cpu(Some(black)).expect("begin");
    c.fill_rrect(8.0, 8.0, 48.0, 48.0, 12.0, red);
    c.end();

    let (w, h, stride, px) = c.read_pixels().expect("CPU backend exposes pixels");
    assert_eq!((w, h), (64, 64));
    assert_eq!(stride, 64 * 4);

    // Centre is inside the rounded rect → opaque red.
    let center = 32 * stride as usize + 32 * 4;
    assert!(px[center] > 250, "center R = {}", px[center]);
    assert!(px[center + 1] < 5 && px[center + 2] < 5);
    assert!(px[center + 3] > 250, "center A = {}", px[center + 3]);

    // A far corner is outside the rounded corner → cleared black.
    let corner = 1 * stride as usize + 1 * 4;
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
