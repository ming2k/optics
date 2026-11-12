//! End-to-end smoke test for the safe bindings: parse, inspect, paint,
//! rasterize, and prove the document drains.
//!
//! Everything here runs headlessly on flux's software canvas, so it needs no GPU
//! and no display — which is the point of the library being device-less.

use reticle::{Document, DrawOptions, Error, Fit, PaintMode, PixelFormat, RasterizeOptions};

const RED_SQUARE: &str =
    "<svg viewBox=\"0 0 10 10\"><rect width=\"10\" height=\"10\" fill=\"#FF0000\"/></svg>";

#[test]
fn version_is_reachable() {
    assert!(!reticle::version().is_empty());
    assert!(reticle::version_check(0, 0, 0));
}

#[test]
fn parses_and_reports_its_viewport() {
    let document = Document::parse(RED_SQUARE).expect("valid document");
    assert_eq!(document.viewport(), [0.0, 0.0, 10.0, 10.0]);
    assert_eq!(document.shape_count(), 1);
    let shape = document.shape(0).expect("shape 0");
    assert!(!shape.commands().is_empty());
    assert_eq!(shape.bounds(), [0.0, 0.0, 10.0, 10.0]);
    assert!(
        document.shape(1).is_none(),
        "out of range is None, not panic"
    );
}

#[test]
fn metadata_attributes_come_back() {
    let document = Document::parse(
        "<svg viewBox=\"0 0 256 256\" data-hotspot-x=\"64\" data-hotspot-y=\"28\">\
         <path d=\"M0 0L10 0L10 10Z\"/></svg>",
    )
    .expect("valid document");
    assert_eq!(document.attribute("data-hotspot-x"), Some("64"));
    assert_eq!(document.attribute("data-hotspot-y"), Some("28"));
    assert_eq!(document.attribute("data-absent"), None);
}

#[test]
fn malformed_input_is_an_error_not_a_partial_document() {
    // Unterminated: the exact shape of a truncated file.
    assert_eq!(
        Document::parse("<svg viewBox=\"0 0 8 8\"><rect width=\"8\" height=\"8\"/>").unwrap_err(),
        Error::InvalidArgument
    );
    assert_eq!(Document::parse("").unwrap_err(), Error::InvalidArgument);
    assert_eq!(
        Document::parse("not markup at all").unwrap_err(),
        Error::InvalidArgument
    );
}

#[test]
fn empty_and_unterminated_spans_are_errors_not_overreads() {
    // An empty slice is the case that once read past the allocation: `length ==
    // 0` used to mean "measure with strlen" at the C entry point, and a zero-length
    // Rust slice carries no terminator to find. Exercised here through the public
    // binding so the guarantee is pinned at the layer consumers actually use.
    assert_eq!(
        Document::parse_bytes(&[], Default::default()).unwrap_err(),
        Error::InvalidArgument
    );

    // Interior NULs are legal when the span is passed by length, so the parser
    // must not secretly measure its input. Measuring would truncate at the NUL to
    // `<svg viewBox="0 0 8 8">`, an unterminated document and therefore an error.
    // Success, with an empty shape list and the viewport intact, is the proof that
    // `length` — not `strlen` — delimited the input.
    let document = Document::parse_bytes(b"<svg viewBox=\"0 0 8 8\">\0</svg>", Default::default())
        .expect("a length-delimited span may contain interior NULs");
    assert_eq!(document.shape_count(), 0);

    // And an unterminated container marker must be refused, not scanned past the
    // end of the slice looking for its close sequence.
    assert_eq!(
        Document::parse_bytes(b"<![CDATA[<svg viewBox=\"0 0 8 8\">", Default::default())
            .unwrap_err(),
        Error::InvalidArgument
    );
}

#[test]
fn limits_are_enforced() {
    let document = "<svg viewBox=\"0 0 8 8\"><rect width=\"1\" height=\"1\"/>\
                    <rect width=\"1\" height=\"1\"/></svg>";
    let limits = reticle::Limits {
        max_shapes: Some(1),
        ..Default::default()
    };
    assert_eq!(
        Document::parse_with_limits(document, limits).unwrap_err(),
        Error::InvalidArgument
    );
}

#[test]
fn monochrome_rasterization_uses_the_theme_colour() {
    let document = Document::parse(RED_SQUARE).expect("valid document");
    let mut options = RasterizeOptions::new(8, 8);
    options.paint = PaintMode::monochrome(0xFF00_FF00);
    let pixmap = document.rasterize(options).expect("rasterize");
    assert_eq!((pixmap.width(), pixmap.height()), (8, 8));
    let pixels = pixmap.as_bytes();
    let center = 4 * pixmap.stride() as usize + 4 * 4;
    // Premultiplied RGBA: opaque green, the document's red discarded.
    assert_eq!(
        &pixels[center..center + 4],
        &[0, 255, 0, 255],
        "monochrome must replace the paint colour"
    );
}

#[test]
fn bgra_is_a_channel_swap_of_the_same_raster() {
    let document = Document::parse(RED_SQUARE).expect("valid document");
    let mut rgba_options = RasterizeOptions::new(8, 8);
    rgba_options.paint = PaintMode::theme(0);
    let rgba = document.rasterize(rgba_options).expect("rgba");
    let mut bgra_options = rgba_options;
    bgra_options.format = PixelFormat::Bgra8Premul;
    let bgra = document.rasterize(bgra_options).expect("bgra");

    let offset = 4 * rgba.stride() as usize + 4 * 4;
    let a = &rgba.as_bytes()[offset..offset + 4];
    let b = &bgra.as_bytes()[offset..offset + 4];
    assert_eq!([b[0], b[1], b[2], b[3]], [a[2], a[1], a[0], a[3]]);
}

#[test]
fn coverage_is_a_single_channel_mask() {
    let document = Document::parse(RED_SQUARE).expect("valid document");
    let mut options = RasterizeOptions::new(8, 8);
    options.format = PixelFormat::Coverage;
    options.paint = PaintMode::monochrome(0xFFFF_FFFF);
    let pixmap = document.rasterize(options).expect("coverage");
    assert_eq!(pixmap.stride(), pixmap.width());
    assert_eq!(
        pixmap.as_bytes()[4 * pixmap.stride() as usize + 4],
        255,
        "interior coverage is full"
    );
}

#[test]
fn drawing_onto_a_canvas_and_an_encoder_agree() {
    let document = Document::parse(RED_SQUARE).expect("valid document");
    let options = DrawOptions {
        bounds: [0.0, 0.0, 8.0, 8.0],
        fit: Fit::Contain,
        paint: PaintMode::monochrome(0xFFFF_FFFF),
        ..Default::default()
    };

    // Canvas path: the pass is open, which is the only window in which a canvas
    // is recordable at all — `Sink::Canvas` takes the pass commands, so the
    // borrow checker enforces that.
    let mut canvas = flux::Canvas::new_cpu(8, 8, 1.0).expect("cpu canvas");
    let session = canvas
        .begin_session(
            (),
            flux::CanvasPassOptions {
                clear: Some(0),
                ..Default::default()
            },
        )
        .expect("begin");
    document
        .draw(reticle::Sink::Canvas(&session), options)
        .expect("record onto canvas");
    session.end().expect("end");
    let (width, height, stride, pixels) = canvas.read_pixels().expect("cpu pixels");
    assert_eq!((width, height), (8, 8));
    let center = 4 * stride as usize + 4 * 4;
    assert_eq!(
        &pixels[center..center + 4],
        &[255, 255, 255, 255],
        "the monochrome silhouette fills the box"
    );

    // Encoder path: same painter, same geometry, replayable list.
    let encoder = flux::Encoder::new().expect("encoder");
    document
        .draw(reticle::Sink::Encoder(&encoder), options)
        .expect("record onto encoder");
    let list = encoder.finish().expect("finish");
    assert!(list.command_count() > 0);
}

#[test]
fn documents_are_shareable_across_threads() {
    let document = Document::parse(RED_SQUARE).expect("valid document");
    let viewport = document.viewport();

    // Sharing is the documented contract: a document is immutable and its
    // refcount is atomic, so a clone may cross threads and be released there.
    let shared = document.clone();
    let handle = std::thread::spawn(move || (shared.shape_count(), shared.viewport()));
    let (shapes, from_thread) = handle.join().expect("thread");
    assert_eq!(shapes, 1);
    assert_eq!(from_thread, viewport);
    // The original outlives the clone dropped on the other thread.
    assert_eq!(document.shape_count(), 1);

    // Exact drain balance is asserted by tests/svg/test_svg_leak.c, which owns
    // the process around the counter. Here it is only observed: cargo runs these
    // `#[test]`s concurrently in one process, so an absolute value is other
    // tests' business.
    assert!(reticle::live_documents() >= 1);
}
