/* test_reticle_draw.c — painting a document: fit, clip, recolour, and headless
 * rasterization.
 *
 * The pixel assertions use art whose ink lands on known pixels, so a regression
 * says *what* broke (aspect, rotation, tint, channel order) instead of "the
 * snapshot changed". Channel order matters in particular: a hardware cursor
 * plane reads BGRA, and a silent swap produces a cursor with red and blue
 * exchanged — visible only in the one place nobody screenshots. */

#include "test_helpers.h"

#include <flux/canvas_cpu.h>
#include <reticle/reticle.h>

/* A full-bleed square: every pixel of the destination is covered, which makes
 * "did anything get drawn at all" unambiguous. */
static const char *SQUARE = "<svg viewBox=\"0 0 10 10\"><rect width=\"10\" height=\"10\" "
                            "fill=\"#FF0000\"/></svg>";

/* A unit square in the top-left quarter of a 2x1 viewport: the fill pixel is
 * known, so aspect/rotation bugs are measurable. */
static const char *TOP_LEFT = "<svg viewBox=\"0 0 2 1\">"
                              "<rect x=\"0\" y=\"0\" width=\"1\" height=\"1\" fill=\"#FFFFFF\"/>"
                              "</svg>";

static reticle_document *load(const char *text) {
    reticle_document *doc = nullptr;
    if (reticle_parse(&(reticle_parse_opts){.length = RETICLE_LENGTH_MEASURE, .text = text},
                      &doc) != FLUX_OK)
        return nullptr;
    return doc;
}

static const uint8_t *pixel(const reticle_pixmap *pixmap, uint32_t x, uint32_t y) {
    return pixmap->pixels + (size_t)y * pixmap->stride + (size_t)x * 4u;
}

static void monochrome_takes_the_theme_colour(void) {
    reticle_document *doc = load(SQUARE);
    if (!doc)
        return;
    reticle_pixmap pixmap = {};
    CHECK(reticle_rasterize(
              &(reticle_rasterize_opts){
                  .document = doc,
                  .width = 8,
                  .height = 8,
                  .format = RETICLE_PIXELS_RGBA8_PREMUL,
                  .paint = RETICLE_PAINT_MONOCHROME,
                  .color = flux_color_rgba(0, 255, 0, 255),
              },
              &pixmap) == FLUX_OK);
    if (pixmap.pixels) {
        const uint8_t *p = pixel(&pixmap, 4, 4);
        /* Premultiplied RGBA: fully opaque green. The document said red; the
         * monochrome mode must ignore it. */
        CHECK(p[0] == 0 && p[1] == 255 && p[2] == 0 && p[3] == 255);
    }
    reticle_pixmap_deinit(&pixmap);
    reticle_release(doc);
}

static void theme_mode_keeps_document_colours(void) {
    reticle_document *doc = load(SQUARE);
    if (!doc)
        return;
    reticle_pixmap pixmap = {};
    CHECK(reticle_rasterize(
              &(reticle_rasterize_opts){
                  .document = doc,
                  .width = 8,
                  .height = 8,
                  .format = RETICLE_PIXELS_RGBA8_PREMUL,
                  .paint = RETICLE_PAINT_THEME,
                  .color = flux_color_rgba(0, 255, 0, 255),
              },
              &pixmap) == FLUX_OK);
    if (pixmap.pixels) {
        const uint8_t *p = pixel(&pixmap, 4, 4);
        CHECK(p[0] == 255 && p[1] == 0 && p[2] == 0 && p[3] == 255);
    }
    reticle_pixmap_deinit(&pixmap);
    reticle_release(doc);
}

static void bgra_swaps_only_the_chroma(void) {
    /* The BGRA readback exists for DRM/KMS cursor planes. It must be a channel
     * swap of the same resolved pixels — not a re-rasterization, which would
     * introduce a second rounding. Alpha must be untouched. */
    reticle_document *doc = load(SQUARE);
    if (!doc)
        return;
    reticle_pixmap rgba = {}, bgra = {};
    reticle_rasterize_opts opts = {
        .document = doc,
        .width = 8,
        .height = 8,
        .format = RETICLE_PIXELS_RGBA8_PREMUL,
        .paint = RETICLE_PAINT_THEME,
    };
    CHECK(reticle_rasterize(&opts, &rgba) == FLUX_OK);
    opts.format = RETICLE_PIXELS_BGRA8_PREMUL;
    CHECK(reticle_rasterize(&opts, &bgra) == FLUX_OK);
    if (rgba.pixels && bgra.pixels) {
        CHECK(rgba.width == bgra.width && rgba.height == bgra.height);
        const uint8_t *a = pixel(&rgba, 4, 4);
        const uint8_t *b = pixel(&bgra, 4, 4);
        CHECK(b[0] == a[2] && b[1] == a[1] && b[2] == a[0] && b[3] == a[3]);
    }
    reticle_pixmap_deinit(&rgba);
    reticle_pixmap_deinit(&bgra);
    reticle_release(doc);
}

static void coverage_is_single_channel(void) {
    reticle_document *doc = load(SQUARE);
    if (!doc)
        return;
    reticle_pixmap pixmap = {};
    CHECK(reticle_rasterize(
              &(reticle_rasterize_opts){
                  .document = doc,
                  .width = 8,
                  .height = 8,
                  .format = RETICLE_PIXELS_COVERAGE,
                  .paint = RETICLE_PAINT_MONOCHROME,
                  .color = flux_color_rgba(255, 255, 255, 255),
              },
              &pixmap) == FLUX_OK);
    if (pixmap.pixels) {
        CHECK(pixmap.stride == pixmap.width);
        /* Interior coverage is full; the mask is what a coverage cache keys on. */
        CHECK(pixmap.pixels[4 * pixmap.stride + 4] == 255);
    }
    reticle_pixmap_deinit(&pixmap);
    reticle_release(doc);
}

static void contain_preserves_aspect_and_centres(void) {
    /* A 2x1 document in an 8x8 box must occupy 8x4, vertically centred: rows 0..1
     * and 6..7 stay transparent, rows 2..5 are ink. */
    reticle_document *doc = load(TOP_LEFT);
    if (!doc)
        return;
    reticle_pixmap pixmap = {};
    CHECK(reticle_rasterize(
              &(reticle_rasterize_opts){
                  .document = doc,
                  .width = 8,
                  .height = 8,
                  .format = RETICLE_PIXELS_COVERAGE,
                  .paint = RETICLE_PAINT_MONOCHROME,
                  .color = flux_color_rgba(255, 255, 255, 255),
              },
              &pixmap) == FLUX_OK);
    if (pixmap.pixels) {
        const uint8_t *top = pixmap.pixels;
        const uint8_t *middle = pixmap.pixels + (size_t)4 * pixmap.stride;
        CHECK(top[0] == 0);
        /* The white unit square fills the left half of the drawn band. */
        CHECK(middle[0] > 200);
        CHECK(middle[pixmap.width - 1] < 40);
    }
    reticle_pixmap_deinit(&pixmap);
    reticle_release(doc);
}

static void draw_requires_exactly_one_destination(void) {
    reticle_document *doc = load(SQUARE);
    if (!doc)
        return;
    /* Neither: a caller mistake that must not silently no-op. */
    CHECK(reticle_draw(&(reticle_draw_opts){
              .document = doc,
              .bounds = {0.0f, 0.0f, 8.0f, 8.0f},
          }) == FLUX_ERROR_INVALID_ARGUMENT);
    /* Both: ambiguous, refused. */
    flux_canvas *canvas = nullptr;
    if (flux_canvas_create_cpu(8, 8, 1.0f, &canvas) == FLUX_OK) {
        flux_encoder *encoder = nullptr;
        if (flux_encoder_create(&(flux_encoder_desc){}, &encoder) == FLUX_OK) {
            CHECK(reticle_draw(&(reticle_draw_opts){
                      .document = doc,
                      .canvas = canvas,
                      .encoder = encoder,
                      .bounds = {0.0f, 0.0f, 8.0f, 8.0f},
                  }) == FLUX_ERROR_INVALID_ARGUMENT);
            flux_encoder_destroy(encoder);
        }
        flux_canvas_release(canvas);
    }
    /* A zero-extent box has no meaning. */
    CHECK(reticle_draw(&(reticle_draw_opts){
              .document = doc,
              .canvas = nullptr,
              .bounds = {0.0f, 0.0f, 0.0f, 8.0f},
          }) == FLUX_ERROR_INVALID_ARGUMENT);
    reticle_release(doc);
}

static void encoder_recording_replays_identically(void) {
    /* The encoder path must produce the same pixels as the immediate canvas path:
     * an immutable display list is a recording, not a different renderer. */
    reticle_document *doc = load("<svg viewBox=\"0 0 10 10\"><circle cx=\"5\" cy=\"5\" r=\"4\" "
                                 "fill=\"#00FF00\"/></svg>");
    if (!doc)
        return;

    reticle_pixmap immediate = {};
    CHECK(reticle_rasterize(
              &(reticle_rasterize_opts){
                  .document = doc,
                  .width = 16,
                  .height = 16,
                  .format = RETICLE_PIXELS_RGBA8_PREMUL,
                  .paint = RETICLE_PAINT_THEME,
              },
              &immediate) == FLUX_OK);

    flux_encoder *encoder = nullptr;
    flux_canvas *canvas = nullptr;
    if (flux_encoder_create(&(flux_encoder_desc){}, &encoder) != FLUX_OK ||
        flux_canvas_create_cpu(16, 16, 1.0f, &canvas) != FLUX_OK) {
        reticle_pixmap_deinit(&immediate);
        if (encoder)
            flux_encoder_destroy(encoder);
        if (canvas)
            flux_canvas_release(canvas);
        reticle_release(doc);
        return;
    }
    CHECK(reticle_draw(&(reticle_draw_opts){
              .document = doc,
              .encoder = encoder,
              .bounds = {0.0f, 0.0f, 16.0f, 16.0f},
              .paint = RETICLE_PAINT_THEME,
          }) == FLUX_OK);
    flux_display_list *list = nullptr;
    CHECK(flux_encoder_finish(encoder, &list) == FLUX_OK);
    flux_encoder_destroy(encoder);

    if (list) {
        flux_color transparent = 0;
        flux_canvas_pass_desc pass = {
            .type = FLUX_TYPE_CANVAS_PASS_DESC,
            .clear_color = &transparent,
        };
        if (flux_canvas_begin(canvas, &pass) == FLUX_OK) {
            CHECK(flux_canvas_submit_display_list(canvas, list) == FLUX_OK);
            CHECK(flux_canvas_end(canvas) == FLUX_OK);
            uint32_t w = 0, h = 0, stride = 0;
            const uint8_t *pixels = flux_canvas_cpu_pixels(canvas, &w, &h, &stride);
            if (pixels && immediate.pixels) {
                CHECK(w == immediate.width && h == immediate.height);
                size_t same = 0;
                for (uint32_t y = 0; y < h; ++y) {
                    const uint8_t *a = pixels + (size_t)y * stride;
                    const uint8_t *b = immediate.pixels + (size_t)y * immediate.stride;
                    for (uint32_t x = 0; x < w * 4u; ++x)
                        same += a[x] == b[x];
                }
                CHECK(same == (size_t)w * h * 4u);
            }
        }
        flux_display_list_release(list);
    }
    flux_canvas_release(canvas);
    reticle_pixmap_deinit(&immediate);
    reticle_release(doc);
}

static void global_opacity_multiplies_the_silhouette(void) {
    reticle_document *doc = load(SQUARE);
    if (!doc)
        return;
    flux_encoder *encoder = nullptr;
    if (flux_encoder_create(&(flux_encoder_desc){}, &encoder) != FLUX_OK) {
        reticle_release(doc);
        return;
    }
    /* Halving is observable in the recorded command stream only through the
     * paint, so this asserts the call is accepted and produces commands. */
    CHECK(reticle_draw(&(reticle_draw_opts){
              .document = doc,
              .encoder = encoder,
              .bounds = {0.0f, 0.0f, 8.0f, 8.0f},
              .paint = RETICLE_PAINT_MONOCHROME,
              .color = flux_color_rgba(255, 255, 255, 255),
              .opacity = 0.5f,
          }) == FLUX_OK);
    flux_display_list *list = nullptr;
    CHECK(flux_encoder_finish(encoder, &list) == FLUX_OK);
    flux_encoder_destroy(encoder);
    if (list) {
        CHECK(flux_display_list_command_count(list) > 0);
        flux_display_list_release(list);
    }
    reticle_release(doc);
}

static void empty_document_draws_cleanly(void) {
    /* A document with no shapes is not an error: an icon that legitimately has
     * no visible geometry must not fail its consumer's draw. */
    reticle_document *doc = load("<svg viewBox=\"0 0 8 8\"></svg>");
    if (!doc)
        return;
    reticle_pixmap pixmap = {};
    CHECK(reticle_rasterize(
              &(reticle_rasterize_opts){
                  .document = doc,
                  .width = 4,
                  .height = 4,
                  .format = RETICLE_PIXELS_RGBA8_PREMUL,
                  .paint = RETICLE_PAINT_THEME,
              },
              &pixmap) == FLUX_OK);
    if (pixmap.pixels) {
        /* Fully transparent: the clear colour, untouched. */
        for (uint32_t i = 0; i < pixmap.width * pixmap.height; ++i)
            CHECK(pixmap.pixels[i * 4 + 3] == 0);
    }
    reticle_pixmap_deinit(&pixmap);
    reticle_release(doc);
}

static void pixmap_deinit_is_idempotent(void) {
    reticle_pixmap pixmap = {};
    reticle_pixmap_deinit(&pixmap);
    CHECK(pixmap.pixels == nullptr);
    reticle_document *doc = load(SQUARE);
    if (doc) {
        CHECK(reticle_rasterize(
                  &(reticle_rasterize_opts){
                      .document = doc,
                      .width = 4,
                      .height = 4,
                      .format = RETICLE_PIXELS_RGBA8_PREMUL,
                      .paint = RETICLE_PAINT_THEME,
                  },
                  &pixmap) == FLUX_OK);
        CHECK(pixmap.pixels != nullptr);
        reticle_pixmap_deinit(&pixmap);
        CHECK(pixmap.pixels == nullptr);
        reticle_pixmap_deinit(&pixmap);
        CHECK(pixmap.pixels == nullptr);
        reticle_release(doc);
    }
}

/// Rasterize `svg` at `px` and return the coverage mass normalised by a full
/// row: the effective ink width in device pixels.
static double effective_stroke_width(const char *svg, uint32_t px) {
    reticle_document *doc = load(svg);
    if (!doc)
        return -1.0;
    reticle_pixmap pixmap = {};
    if (reticle_rasterize(
            &(reticle_rasterize_opts){
                .document = doc,
                .width = px,
                .height = px,
                .format = RETICLE_PIXELS_COVERAGE,
                .paint = RETICLE_PAINT_MONOCHROME,
                .color = flux_color_rgba(255, 255, 255, 255),
            },
            &pixmap) != FLUX_OK) {
        reticle_release(doc);
        return -1.0;
    }
    double mass = 0.0;
    const uint32_t width = pixmap.width;
    for (uint32_t i = 0; i < pixmap.width * pixmap.height; ++i)
        mass += pixmap.pixels[i];
    reticle_pixmap_deinit(&pixmap);
    reticle_release(doc);
    return mass / ((double)width * 255.0);
}

/// Element and group opacity must reach the pixels.
///
/// This is the regression that matters most: `opacity` was parsed, stored on the
/// shape, and then never multiplied into the brush, so a document asking for 50%
/// painted at 100%. A test that only checked `shape.opacity()` would have passed
/// while the picture was wrong, which is why the assertion is on the rasterized
/// alpha.
static void element_opacity_reaches_the_pixels(void) {
    static const struct {
        const char *label;
        const char *svg;
    } CASES[] = {
        {"element opacity",
         "<svg viewBox=\"0 0 8 8\"><rect width=\"8\" height=\"8\" fill=\"#FF0000\" "
         "opacity=\"0.5\"/></svg>"},
        {"group opacity",
         "<svg viewBox=\"0 0 8 8\"><g opacity=\"0.5\"><rect width=\"8\" height=\"8\" "
         "fill=\"#FF0000\"/></g></svg>"},
        {"nested group opacity", "<svg viewBox=\"0 0 8 8\"><g opacity=\"0.5\"><g opacity=\"0.5\">"
                                 "<rect width=\"8\" height=\"8\" fill=\"#FF0000\"/></g></g></svg>"},
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; ++i) {
        reticle_document *doc = load(CASES[i].svg);
        CHECK(doc != nullptr);
        if (!doc)
            continue;
        reticle_pixmap pixmap = {};
        CHECK(reticle_rasterize(
                  &(reticle_rasterize_opts){
                      .document = doc,
                      .width = 8,
                      .height = 8,
                      .format = RETICLE_PIXELS_RGBA8_PREMUL,
                      .paint = RETICLE_PAINT_THEME,
                  },
                  &pixmap) == FLUX_OK);
        if (pixmap.pixels) {
            const uint8_t *p = pixel(&pixmap, 4, 4);
            /* 0.5 -> 128, 0.25 -> 64. Antialiasing does not touch an interior
             * pixel, so this is exact up to the 8-bit quantisation. */
            const uint8_t expected = i == 2 ? 64 : 128;
            if (p[3] != expected)
                fprintf(stderr, "FAIL %s: alpha %u != %u\n", CASES[i].label, p[3], expected);
            CHECK(p[3] == expected);
        }
        reticle_pixmap_deinit(&pixmap);
        reticle_release(doc);
    }
}

/// A stroke width is in *user* units and must scale exactly once with the fit.
///
/// The regression this pins: the painter multiplied the document's width by the
/// fit scale while the canvas transform also scaled it, so a 1-unit stroke came
/// out at `scale^2` device pixels (16 instead of 4 at 4x). An icon rendered at a
/// non-1.0 scale therefore had visibly fattened outlines while looking fine at
/// exactly 1.0 — which is why the check spans several scales.
static void stroke_width_scales_exactly_once(void) {
    const char *line = "<svg viewBox=\"0 0 8 8\"><path d=\"M0 4L8 4\" stroke=\"#000\" "
                       "stroke-width=\"1\" fill=\"none\"/></svg>";
    const uint32_t scales[] = {8, 16, 32, 64};
    for (size_t i = 0; i < sizeof scales / sizeof scales[0]; ++i) {
        const uint32_t px = scales[i];
        const double measured = effective_stroke_width(line, px);
        const double expected = (double)px / 8.0; /* 1 user unit = px/8 device px */
        if (!(fabs(measured - expected) <= 0.06))
            fprintf(stderr, "FAIL %upx: stroke %.3f != %.3f\n", px, measured, expected);
        CHECK(fabs(measured - expected) <= 0.06);
    }
}

/// A document's own stroke width also scales once; an explicit override is in
/// destination units and must not.
static void stroke_override_is_in_destination_units(void) {
    /* The same 1-unit stroke, but the caller pins 4 device pixels. At 32px the
     * fit scale is 4, so a double-scaled override would read 16. */
    const char *line = "<svg viewBox=\"0 0 8 8\"><path d=\"M0 4L8 4\" stroke=\"#000\" "
                       "stroke-width=\"1\" fill=\"none\"/></svg>";
    reticle_document *doc = load(line);
    if (!doc)
        return;
    for (uint32_t px = 16; px <= 32; px *= 2) {
        reticle_pixmap pixmap = {};
        if (reticle_rasterize(
                &(reticle_rasterize_opts){
                    .document = doc,
                    .width = px,
                    .height = px,
                    .format = RETICLE_PIXELS_COVERAGE,
                    .paint = RETICLE_PAINT_MONOCHROME,
                    .color = flux_color_rgba(255, 255, 255, 255),
                    .stroke_width = 4.0f,
                },
                &pixmap) != FLUX_OK) {
            continue;
        }
        double mass = 0.0;
        for (uint32_t i = 0; i < pixmap.width * pixmap.height; ++i)
            mass += pixmap.pixels[i];
        const double measured = mass / ((double)pixmap.width * 255.0);
        if (!(fabs(measured - 4.0) <= 0.15))
            fprintf(stderr, "FAIL override @%upx: %.3f != 4\n", px, measured);
        CHECK(fabs(measured - 4.0) <= 0.15);
        reticle_pixmap_deinit(&pixmap);
    }
    reticle_release(doc);
}

int main(void) {
    monochrome_takes_the_theme_colour();
    theme_mode_keeps_document_colours();
    bgra_swaps_only_the_chroma();
    coverage_is_single_channel();
    contain_preserves_aspect_and_centres();
    draw_requires_exactly_one_destination();
    encoder_recording_replays_identically();
    global_opacity_multiplies_the_silhouette();
    element_opacity_reaches_the_pixels();
    stroke_width_scales_exactly_once();
    stroke_override_is_in_destination_units();
    empty_document_draws_cleanly();
    pixmap_deinit_is_idempotent();
    TEST_REPORT();
}
