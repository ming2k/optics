//! Headless software-canvas smoke test: exercises the CPU backend end to end
//! from the safe Rust API with no GPU, device, or window.

use flux::{CanvasPassOptions, Canvas, Encoder, GradientStop, Target, rgba};

#[test]
fn cpu_canvas_renders_and_reads_back() {
    let mut c = Canvas::new_cpu(64, 64, 1.0).expect("create CPU canvas");

    let black = rgba(0, 0, 0, 255);
    let red = rgba(255, 0, 0, 255);

    let session = c.begin_session((), CanvasPassOptions { clear: Some(black), ..Default::default() }).expect("begin");
    session.fill_rrect(8.0, 8.0, 48.0, 48.0, 12.0, red);
    session.end().expect("end checked CPU pass");

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
    let mut c = Canvas::new_cpu(32, 32, 1.0).unwrap();
    let session = c.begin_session((), CanvasPassOptions { clear: Some(rgba(0, 0, 0, 255)), ..Default::default() }).unwrap();
    session.fill_rect(0.0, 0.0, 32.0, 32.0, rgba(0, 0, 255, 255));
    session.end().unwrap();
    let (_, _, stride, px) = c.read_pixels().unwrap();
    let p = 16 * stride as usize + 16 * 4;
    assert!(px[p + 2] > 250 && px[p] < 5); // blue
}

#[test]
fn rgba_premultiplies_translucent_colours_for_src_over() {
    let mut c = Canvas::new_cpu(1, 1, 1.0).unwrap();
    let session = c.begin_session((), CanvasPassOptions { clear: Some(rgba(0, 0, 0, 255)), ..Default::default() }).unwrap();
    session.fill_rect(0.0, 0.0, 1.0, 1.0, rgba(255, 255, 255, 32));
    session.end().unwrap();
    let (_, _, _, pixels) = c.read_pixels().expect("CPU readback");
    // ADR-0069: blending happens in the linear-light working space, so the
    // result is sRGB-encoded on output: srgb_encode(32/255) * 255 ~= 99,
    // not the 32 of the old gamma-space pipeline.
    assert!(
        (95..=103).contains(&pixels[0]),
        "translucent white over black should encode linear 32/255 to ~99, got {}",
        pixels[0]
    );
    assert_eq!(pixels[0], pixels[1]);
    assert_eq!(pixels[1], pixels[2]);
    assert_eq!(pixels[3], 255);
}

#[test]
fn safe_radial_gradient_reaches_canvas_backend() {
    let mut c = Canvas::new_cpu(32, 32, 1.0).unwrap();
    let session = c.begin_session((), CanvasPassOptions { clear: Some(rgba(0, 0, 0, 255)), ..Default::default() }).unwrap();
    session.fill_rect_radial_gradient(
        (0.0, 0.0, 32.0, 32.0),
        (16.0, 16.0),
        16.0,
        &[
            GradientStop::new(0.0, rgba(255, 64, 32, 255)),
            GradientStop::new(1.0, rgba(255, 64, 32, 0)),
        ],
    );
    session.end().unwrap();
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
    let mut c = Canvas::new_cpu(128, 128, 1.0).unwrap();
    let session = c.begin_session((), CanvasPassOptions { clear: Some(rgba(0, 0, 0, 255)), ..Default::default() }).unwrap();
    session.save();
    session.scale(2.0, 2.0);
    session.fill_rect_radial_gradient(
        (0.0, 0.0, 64.0, 64.0),
        (32.0, 32.0),
        16.0,
        &[
            GradientStop::new(0.0, rgba(255, 64, 32, 255)),
            GradientStop::new(1.0, rgba(255, 64, 32, 0)),
        ],
    );
    session.restore();
    session.end().unwrap();
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
    let mut c = Canvas::new_cpu(128, 128, 1.0).unwrap();
    let session = c.begin_session((), CanvasPassOptions { clear: Some(rgba(0, 0, 0, 255)), ..Default::default() }).unwrap();
    session.save();
    session.scale(2.0, 2.0);
    session.fill_rect_linear_gradient(
        (0.0, 0.0, 64.0, 64.0),
        (0.0, 0.0),
        (64.0, 0.0),
        &[
            GradientStop::new(0.0, rgba(255, 0, 0, 255)),
            GradientStop::new(1.0, rgba(255, 0, 0, 0)),
        ],
    );
    session.restore();
    session.end().unwrap();
    let (_, _, stride, pixels) = c.read_pixels().expect("CPU readback");
    let near_start = 64 * stride as usize + 8 * 4;
    let mid = 64 * stride as usize + 64 * 4;
    assert!(
        pixels[near_start] > 200,
        "device x=8 should still be near full red, R = {}",
        pixels[near_start]
    );
    assert!(
        (183..=192).contains(&pixels[mid]),
        // ADR-0069: stops interpolate in linear light, so the midpoint is
        // linear 0.5, sRGB-encoded to ~187 on output (not the ~128 of the
        // old gamma-space pipeline).
        "device x=64 should sit mid-gradient, R = {}",
        pixels[mid]
    );
}

#[test]
fn pixel_snapshot_survives_subsequent_frames() {
    let mut c = Canvas::new_cpu(8, 8, 1.0).unwrap();
    let session = c.begin_session((), CanvasPassOptions { clear: Some(rgba(255, 0, 0, 255)), ..Default::default() }).unwrap();
    session.end().unwrap();
    let (_, _, _, first) = c.read_pixels().unwrap();
    let session = c.begin_session((), CanvasPassOptions { clear: Some(rgba(0, 0, 255, 255)), ..Default::default() }).unwrap();
    session.end().unwrap();
    let (_, _, _, second) = c.read_pixels().unwrap();
    assert_eq!(&first[..4], &[255, 0, 0, 255]);
    assert_eq!(&second[..4], &[0, 0, 255, 255]);
}

#[test]
fn canvas_session_renders_to_cpu_target() {
    let mut target = Target::create_cpu(32, 32).expect("create CPU target");
    let mut c = Canvas::new_cpu(32, 32, 1.0).expect("create CPU canvas");

    let green = rgba(0, 255, 0, 255);
    let red = rgba(255, 0, 0, 255);

    let session = c.begin_session(&mut target, CanvasPassOptions { clear: Some(green), ..Default::default() }).expect("begin session");
    session.fill_rect(8.0, 8.0, 16.0, 16.0, red);
    session.end().expect("end session cleanly");

    let pixels = target.cpu_pixels().expect("read CPU target pixels");
    assert_eq!(pixels.len(), 32 * 32 * 4);

    // Corner at (0, 0) should be cleared green
    assert_eq!(&pixels[..4], &[0, 255, 0, 255]);

    // Center at (16, 16) should be filled red
    let center_idx = (16 * 32 + 16) * 4;
    assert_eq!(&pixels[center_idx..center_idx + 4], &[255, 0, 0, 255]);
}

#[test]
fn encoder_display_list_playback_to_target() {
    let mut target = Target::create_cpu(32, 32).expect("create CPU target");
    let mut c = Canvas::new_cpu(32, 32, 1.0).expect("create CPU canvas");

    let mut enc = Encoder::new().expect("create encoder");
    enc.save_layer(None, 0.5);
    enc.translate(4.0, 4.0);
    enc.clip_rect((0.0, 0.0, 16.0, 16.0));
    enc.restore();
    let dl = enc.finish().expect("finish display list");

    assert!(dl.command_count() >= 4);
    assert!(dl.size() > 0);

    let cloned_dl = dl.clone();
    drop(dl);

    let session = c.begin_session(&mut target, CanvasPassOptions { clear: Some(rgba(0, 0, 0, 255)), ..Default::default() }).expect("begin session");
    session.submit_display_list(&cloned_dl).expect("submit display list");
    session.end().expect("end session");

    let pixels = target.cpu_pixels().expect("read CPU target pixels");
    assert_eq!(pixels.len(), 32 * 32 * 4);
}
