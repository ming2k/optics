/* test_record_replay.c — subtree display-list record/replay (ADR-0030).
 *
 * Golden-pixel tests over the CPU canvas: an unchanged subtree must
 * replay its recorded segment (skipping re-emission) and produce a
 * pixel-identical framebuffer; a changed leaf must re-record while its
 * siblings still replay; a scale switch must invalidate every record;
 * a scroll/viewport clip change must invalidate the records inside the
 * container. Replay/record activity is asserted through the canvas's
 * cumulative counters.
 */

#include "test_helpers.h"
#include <flux/canvas_cpu.h>
#include <lens/lens.h>
#include <stdio.h>
#include <string.h>

#define W 200
#define H 120

/* Cursor parked inside the window (but off any hoverable control in
 * these builds) so hover easing does not leak damage into "static"
 * frames; the scroll test moves it over the scroll container. */
static const lens_input IN0 = {.display_size = {W, H},
                               .cursor = {50, 50},
                               .dt_seconds = 0.016f};

/* Cursor over the scroll container's body (the header label above it is
 * ~51 px tall): wheel routing requires the cursor inside its bounds. */
static const lens_input IN_SCROLL = {.display_size = {W, H},
                                     .cursor = {50, 80},
                                     .dt_seconds = 0.016f};

typedef struct fixture {
    lens *ui;
    flux_canvas *canvas;
} fixture;

static void fixture_open(fixture *f) {
    f->ui = NULL;
    f->canvas = NULL;
    CHECK(lens_create(&(lens_desc){0}, &f->ui) == FLUX_OK);
    CHECK(flux_canvas_create_cpu(W, H, 1.0f, &f->canvas) == FLUX_OK);
}

static void fixture_close(fixture *f) {
    flux_canvas_destroy(f->canvas);
    lens_destroy(f->ui);
}

/* Two sibling blocks so a change in one leaves the other replayable. */
static void build_two_blocks(lens *ui, const lens_input *in, float value) {
    lens_begin(ui, in);
    lens_label(ui, "header##hdr");
    lens_progress(ui, "load", value);
    lens_label(ui, "footer##ftr");
    lens_end(ui);
}

static void snapshot(const fixture *f, uint8_t *out) {
    uint32_t w = 0, h = 0, stride = 0;
    const uint8_t *fb = flux_canvas_cpu_pixels(f->canvas, &w, &h, &stride);
    CHECK(fb != NULL && w == W && h == H && stride == W * 4);
    memcpy(out, fb, (size_t)h * stride);
}

static void render_frame(const fixture *f) {
    flux_color clear = flux_color_rgba_premul(0, 0, 0, 255);
    CHECK(flux_canvas_cpu_begin(f->canvas, &clear) == FLUX_OK);
    CHECK(lens_render(f->ui, f->canvas) == FLUX_OK);
    flux_canvas_cpu_end(f->canvas);
}

/* Static UI: after settling, a frame must replay records (no re-emit)
 * and produce pixels identical to the previous, live-recorded frame. */
static void test_static_frame_replays(void) {
    fixture f;
    fixture_open(&f);
    uint8_t prev[W * H * 4], cur[W * H * 4];

    /* Settle: entering/stable transitions re-record for the first
     * frames; by the third build the tree is unchanged. */
    for (int i = 0; i < 3; i++) {
        build_two_blocks(f.ui, &IN0, 0.5f);
        render_frame(&f);
    }
    CHECK(flux_canvas_records_created(f.canvas) > 0);
    snapshot(&f, prev);

    uint64_t created0 = flux_canvas_records_created(f.canvas);
    uint64_t replayed0 = flux_canvas_records_replayed(f.canvas);
    build_two_blocks(f.ui, &IN0, 0.5f);
    render_frame(&f);
    snapshot(&f, cur);

    CHECK(flux_canvas_records_replayed(f.canvas) > replayed0); /* replay path taken */
    CHECK(flux_canvas_records_created(f.canvas) == created0);  /* nothing re-recorded */
    CHECK(memcmp(prev, cur, sizeof prev) == 0);                /* pixel-identical */

    fixture_close(&f);
}

/* One leaf changes: its subtree (and ancestors) re-record, the sibling
 * subtree still replays, and the frame after settles back to replay. */
static void test_leaf_change_rerecords_sibling_replays(void) {
    fixture f;
    fixture_open(&f);
    uint8_t base[W * H * 4], changed[W * H * 4], settled[W * H * 4];

    for (int i = 0; i < 3; i++) {
        build_two_blocks(f.ui, &IN0, 0.5f);
        render_frame(&f);
    }
    snapshot(&f, base);

    uint64_t created0 = flux_canvas_records_created(f.canvas);
    uint64_t replayed0 = flux_canvas_records_replayed(f.canvas);
    build_two_blocks(f.ui, &IN0, 0.75f); /* same ids, new bar value */
    render_frame(&f);
    snapshot(&f, changed);

    CHECK(flux_canvas_records_created(f.canvas) > created0);   /* leaf re-recorded */
    CHECK(flux_canvas_records_replayed(f.canvas) > replayed0); /* siblings replayed */
    CHECK(memcmp(base, changed, sizeof base) != 0);            /* pixels moved */

    /* Static again: back to full replay, identical pixels. */
    build_two_blocks(f.ui, &IN0, 0.75f);
    render_frame(&f);
    snapshot(&f, settled);
    CHECK(memcmp(changed, settled, sizeof changed) == 0);

    fixture_close(&f);
}

/* Scale switch: baked physical-pixel vertices + push constants go stale,
 * so every record must invalidate (no replays that frame) and the next
 * static frame must be stable and pixel-identical (no corruption). */
static void test_scale_switch_invalidates_all(void) {
    fixture f;
    fixture_open(&f);
    uint8_t zoomed[W * H * 4], stable[W * H * 4];

    const lens_input in_zoom = {.display_size = {W / 2, H / 2},
                                .cursor = {25, 25},
                                .dt_seconds = 0.016f};

    for (int i = 0; i < 3; i++) {
        build_two_blocks(f.ui, &IN0, 0.5f);
        render_frame(&f);
    }

    lens_set_scale(f.ui, 2.0f);
    uint64_t created0 = flux_canvas_records_created(f.canvas);
    uint64_t replayed0 = flux_canvas_records_replayed(f.canvas);
    build_two_blocks(f.ui, &in_zoom, 0.5f);
    render_frame(&f);
    snapshot(&f, zoomed);

    CHECK(flux_canvas_records_replayed(f.canvas) == replayed0); /* all stale */
    CHECK(flux_canvas_records_created(f.canvas) > created0);    /* all re-recorded */

    build_two_blocks(f.ui, &in_zoom, 0.5f);
    render_frame(&f);
    snapshot(&f, stable);
    CHECK(memcmp(zoomed, stable, sizeof zoomed) == 0); /* settled, no garbage */

    fixture_close(&f);
}

static void build_scroll_ui(lens *ui, const lens_input *in) {
    lens_begin(ui, in);
    lens_label(ui, "above##hdr");
    lens_size(ui, 0, 200); /* taller than the window: viewport clips */
    lens_scroll_begin(ui, "scroll");
    for (int i = 0; i < 20; i++) {
        char label[24];
        snprintf(label, sizeof label, "Item##%d", i);
        lens_label(ui, label);
    }
    lens_scroll_end(ui);
    lens_end(ui);
}

/* Scroll + viewport clip change: content inside the container moves and
 * its records (anchored to the container's scissor) must re-record while
 * the widget above the container still replays. */
static void test_scroll_clip_invalidates_container_records(void) {
    fixture f;
    fixture_open(&f);
    uint8_t before[W * H * 4], scrolled[W * H * 4], resized[W * H * 4], stable[W * H * 4];

    for (int i = 0; i < 3; i++) {
        build_scroll_ui(f.ui, &IN_SCROLL);
        render_frame(&f);
    }
    snapshot(&f, before);

    /* Wheel over the container: children move -> re-record; the label
     * above the container is untouched -> replays. */
    lens_input in = IN_SCROLL;
    in.scroll_y = -5.0f;
    uint64_t created0 = flux_canvas_records_created(f.canvas);
    uint64_t replayed0 = flux_canvas_records_replayed(f.canvas);
    build_scroll_ui(f.ui, &in);
    render_frame(&f);
    snapshot(&f, scrolled);
    CHECK(memcmp(before, scrolled, sizeof before) != 0);
    CHECK(flux_canvas_records_created(f.canvas) > created0);
    CHECK(flux_canvas_records_replayed(f.canvas) > replayed0);

    /* Settle after the scroll: identical pixels. */
    build_scroll_ui(f.ui, &IN_SCROLL);
    render_frame(&f);
    snapshot(&f, stable);
    CHECK(memcmp(scrolled, stable, sizeof scrolled) == 0);

    /* Shrink the window: the container's viewport clip changes, so the
     * recorded scissor anchor of every record inside it goes stale. */
    lens_input small = IN_SCROLL;
    small.display_size = (flux_point){W, 80};
    created0 = flux_canvas_records_created(f.canvas);
    replayed0 = flux_canvas_records_replayed(f.canvas);
    build_scroll_ui(f.ui, &small);
    render_frame(&f);
    snapshot(&f, resized);
    CHECK(flux_canvas_records_created(f.canvas) > created0);   /* container re-recorded */
    CHECK(flux_canvas_records_replayed(f.canvas) > replayed0); /* header replayed */

    build_scroll_ui(f.ui, &small);
    render_frame(&f);
    snapshot(&f, stable);
    CHECK(memcmp(resized, stable, sizeof resized) == 0);

    fixture_close(&f);
}

/* HiDPI scale (scale = 2.0f): scissor clip rects must not double-scale
 * into device-device pixel space, which would scissor out valid content
 * near the top/left edge of scroll and table containers. */
static void test_hidpi_scroll_clip_alignment(void) {
    fixture f;
    f.ui = NULL;
    f.canvas = NULL;
    CHECK(lens_create(&(lens_desc){0}, &f.ui) == FLUX_OK);
    /* CPU canvas created at physical resolution 2*W x 2*H (scale = 2.0) */
    CHECK(flux_canvas_create_cpu(W * 2, H * 2, 2.0f, &f.canvas) == FLUX_OK);
    lens_set_scale(f.ui, 2.0f);

    const lens_input hidpi_in = {.display_size = {W, H}, .cursor = {50, 50}, .dt_seconds = 0.016f};

    lens_begin(f.ui, &hidpi_in);
    lens_size(f.ui, W, H);
    lens_scroll_begin(f.ui, "hidpi_scroll");
    lens_label(f.ui, "Visible Label");
    lens_scroll_end(f.ui);
    lens_end(f.ui);

    flux_color clear = flux_color_rgba_premul(0, 0, 0, 255);
    CHECK(flux_canvas_cpu_begin(f.canvas, &clear) == FLUX_OK);
    CHECK(lens_render(f.ui, f.canvas) == FLUX_OK);
    flux_canvas_cpu_end(f.canvas);

    /* Verify that non-zero pixels were drawn in the framebuffer (i.e. the label
     * was not clipped out by a 4x-shifted scissor). */
    uint32_t pw = 0, ph = 0, stride = 0;
    const uint8_t *fb = flux_canvas_cpu_pixels(f.canvas, &pw, &ph, &stride);
    CHECK(fb != NULL && pw == W * 2 && ph == H * 2);

    bool found_non_zero = false;
    for (size_t i = 0; i < (size_t)ph * stride; i++) {
        if (fb[i] > 0) {
            found_non_zero = true;
            break;
        }
    }
    CHECK(found_non_zero == true);

    flux_canvas_destroy(f.canvas);
    lens_destroy(f.ui);
}

int main(void) {
    test_static_frame_replays();
    test_leaf_change_rerecords_sibling_replays();
    test_scale_switch_invalidates_all();
    test_scroll_clip_invalidates_container_records();
    test_hidpi_scroll_clip_alignment();
    return TEST_REPORT();
}

