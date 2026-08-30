/* test_record_replay.c — subtree display-list record/replay (ADR-0030). */

#include "test_helpers.h"
#include <flux/canvas_cpu.h>
#include <lens/lens.h>
#include <stdio.h>
#include <string.h>

#define W 400
#define H 300

static const lens_input IN0 = {.display_size = {W, H}, .dt_seconds = 0.016f};

typedef struct {
    lens *ui;
    flux_canvas *canvas;
} fixture;

static void fixture_open(fixture *f) {
    CHECK(lens_create(&(lens_desc){0}, &f->ui) == FLUX_OK);
    CHECK(flux_canvas_create_cpu(W, H, 1.0f, &f->canvas) == FLUX_OK);
}

static void fixture_close(fixture *f) {
    flux_canvas_destroy(f->canvas);
    lens_destroy(f->ui);
}

static void render_frame(fixture *f) {
    flux_color clear = flux_color_rgba_premul(0, 0, 0, 255);
    CHECK(flux_canvas_cpu_begin(f->canvas, &clear) == FLUX_OK);
    CHECK(lens_render(f->ui, f->canvas) == FLUX_OK);
    flux_canvas_cpu_end(f->canvas);
}

static void snapshot(fixture *f, uint8_t *out) {
    uint32_t pw = 0, ph = 0, stride = 0;
    const uint8_t *fb = flux_canvas_cpu_pixels(f->canvas, &pw, &ph, &stride);
    CHECK(fb != NULL && pw == W && ph == H && stride == W * 4);
    memcpy(out, fb, (size_t)W * H * 4);
}

/* Two sibling blocks so a change in one leaves the other replayable. */
static void build_two_blocks(lens *ui, const lens_input *in, float value) {
    char val_str[32];
    snprintf(val_str, sizeof val_str, "val=%.2f", (double)value);
    lens_begin(ui, in);
    lens_label(ui, &(lens_label_opts){.text = "header##hdr"});
    lens_label(ui, &(lens_label_opts){.text = val_str, .box = {.id = "val_node"}});
    lens_label(ui, &(lens_label_opts){.text = "footer##ftr"});
    lens_end(ui);
}

/* Three identical frames: first frame records, subsequent frames replay
 * without recording new display lists, and every frame produces identical
 * pixels. */
static void test_steady_frame_replays(void) {
    fixture f;
    fixture_open(&f);
    uint8_t base[W * H * 4], frame1[W * H * 4], frame2[W * H * 4];

    /* Warm-up frame: first render records commands */
    build_two_blocks(f.ui, &IN0, 0.5f);
    render_frame(&f);
    snapshot(&f, base);
    uint64_t created0 = flux_canvas_records_created(f.canvas);
    uint64_t replayed0 = flux_canvas_records_replayed(f.canvas);
    CHECK(created0 > 0);

    /* Frame 1: nothing changed -> replay path */
    build_two_blocks(f.ui, &IN0, 0.5f);
    render_frame(&f);
    snapshot(&f, frame1);
    CHECK(memcmp(base, frame1, sizeof base) == 0);
    CHECK(flux_canvas_records_created(f.canvas) == created0);
    CHECK(flux_canvas_records_replayed(f.canvas) > replayed0);

    /* Frame 2: still unchanged -> still replaying identical pixels */
    build_two_blocks(f.ui, &IN0, 0.5f);
    render_frame(&f);
    snapshot(&f, frame2);
    CHECK(memcmp(base, frame2, sizeof base) == 0);
    CHECK(flux_canvas_records_created(f.canvas) == created0);

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

    /* Mutate only the value label */
    uint64_t created_before = flux_canvas_records_created(f.canvas);
    build_two_blocks(f.ui, &IN0, 0.75f);
    render_frame(&f);
    snapshot(&f, changed);
    CHECK(memcmp(base, changed, sizeof base) != 0); /* pixels changed */
    CHECK(flux_canvas_records_created(f.canvas) > created_before);

    /* Settle on the new value: replays the newly-recorded state */
    uint64_t created_settled = flux_canvas_records_created(f.canvas);
    uint64_t replayed_settled = flux_canvas_records_replayed(f.canvas);
    build_two_blocks(f.ui, &IN0, 0.75f);
    render_frame(&f);
    snapshot(&f, settled);
    CHECK(memcmp(changed, settled, sizeof changed) == 0);
    CHECK(flux_canvas_records_created(f.canvas) == created_settled);
    CHECK(flux_canvas_records_replayed(f.canvas) > replayed_settled);

    fixture_close(&f);
}

/* Scale change invalidates every recorded display list across the tree. */
static void test_scale_change_invalidates_all_records(void) {
    fixture f;
    fixture_open(&f);

    for (int i = 0; i < 3; i++) {
        build_two_blocks(f.ui, &IN0, 0.5f);
        render_frame(&f);
    }

    uint64_t created_before = flux_canvas_records_created(f.canvas);
    lens_set_scale(f.ui, 2.0f);
    build_two_blocks(f.ui, &IN0, 0.5f);
    render_frame(&f);
    CHECK(flux_canvas_records_created(f.canvas) > created_before);

    fixture_close(&f);
}

/* Scroll / viewport-translation changes must invalidate the recorded
 * display lists inside the scroll container. */
static void test_scroll_offset_invalidates_descendant_records(void) {
    fixture f;
    fixture_open(&f);

    /* Build a scroll container with several items */
    lens_input in_init = IN0;
    in_init.cursor = (flux_point){50, 50};
    for (int i = 0; i < 3; i++) {
        lens_begin(f.ui, &in_init);
        lens_size(f.ui, W, 100);
        lens_scroll_begin(f.ui, &(lens_scroll_opts){.box = {.id = "scroll"}});
        for (int j = 0; j < 15; j++) {
            char lbl[32];
            snprintf(lbl, sizeof lbl, "item %d", j);
            lens_size(f.ui, 0, 30);
            lens_label(f.ui, &(lens_label_opts){.text = lbl});
        }
        lens_scroll_end(f.ui);
        lens_end(f.ui);
        render_frame(&f);
    }

    /* Scrolling moves the translation: records inside must re-record */
    uint64_t created_before = flux_canvas_records_created(f.canvas);
    lens_input in_scrolled = in_init;
    in_scrolled.scroll_y = -3.0f;
    lens_begin(f.ui, &in_scrolled);
    lens_size(f.ui, W, 100);
    lens_scroll_begin(f.ui, &(lens_scroll_opts){.box = {.id = "scroll"}});
    for (int j = 0; j < 15; j++) {
        char lbl[32];
        snprintf(lbl, sizeof lbl, "item %d", j);
        lens_size(f.ui, 0, 30);
        lens_label(f.ui, &(lens_label_opts){.text = lbl});
    }
    lens_scroll_end(f.ui);
    lens_end(f.ui);
    render_frame(&f);
    CHECK(flux_canvas_records_created(f.canvas) > created_before);

    fixture_close(&f);
}

static void test_hidpi_scroll_clip_alignment(void) {
    fixture f;
    CHECK(lens_create(&(lens_desc){0}, &f.ui) == FLUX_OK);
    CHECK(flux_canvas_create_cpu(W * 2, H * 2, 2.0f, &f.canvas) == FLUX_OK);
    lens_set_scale(f.ui, 2.0f);

    const lens_input hidpi_in = {.display_size = {W, H}, .cursor = {50, 50}, .dt_seconds = 0.016f};

    lens_begin(f.ui, &hidpi_in);
    lens_size(f.ui, W, H);
    lens_scroll_begin(f.ui, &(lens_scroll_opts){.box = {.id = "hidpi_scroll"}});
    lens_label(f.ui, &(lens_label_opts){.text = "Visible Label"});
    lens_scroll_end(f.ui);
    lens_end(f.ui);

    flux_color clear = flux_color_rgba_premul(0, 0, 0, 255);
    CHECK(flux_canvas_cpu_begin(f.canvas, &clear) == FLUX_OK);
    CHECK(lens_render(f.ui, f.canvas) == FLUX_OK);
    flux_canvas_cpu_end(f.canvas);

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

static void test_child_removal_invalidates_record(void) {
    fixture f;
    fixture_open(&f);
    uint8_t full[W * H * 4], shrunk[W * H * 4], reference[W * H * 4], stable[W * H * 4];

    for (int i = 0; i < 3; i++) {
        build_two_blocks(f.ui, &IN0, 0.5f);
        render_frame(&f);
    }
    snapshot(&f, full);

    uint64_t created0 = flux_canvas_records_created(f.canvas);
    lens_begin(f.ui, &IN0);
    lens_label(f.ui, &(lens_label_opts){.text = "header##hdr"});
    lens_label(f.ui, &(lens_label_opts){.text = "val=0.50", .box = {.id = "val_node"}});
    lens_end(f.ui);
    render_frame(&f);
    snapshot(&f, shrunk);
    CHECK(flux_canvas_records_created(f.canvas) > created0);
    CHECK(memcmp(full, shrunk, sizeof full) != 0);

    {
        fixture g;
        fixture_open(&g);
        for (int i = 0; i < 3; i++) {
            lens_begin(g.ui, &IN0);
            lens_label(g.ui, &(lens_label_opts){.text = "header##hdr"});
            lens_label(g.ui, &(lens_label_opts){.text = "val=0.50", .box = {.id = "val_node"}});
            lens_end(g.ui);
            render_frame(&g);
        }
        snapshot(&g, reference);
        fixture_close(&g);
    }
    CHECK(memcmp(shrunk, reference, sizeof shrunk) == 0);

    created0 = flux_canvas_records_created(f.canvas);
    lens_begin(f.ui, &IN0);
    lens_end(f.ui);
    render_frame(&f);
    snapshot(&f, stable);
    CHECK(flux_canvas_records_created(f.canvas) > created0);
    bool any_ink = false;
    for (size_t i = 0; i + 2 < sizeof stable; i += 4) {
        if (stable[i] || stable[i + 1] || stable[i + 2]) {
            any_ink = true;
            break;
        }
    }
    CHECK(!any_ink);

    for (int i = 0; i < 3; i++) {
        build_two_blocks(f.ui, &IN0, 0.5f);
        render_frame(&f);
    }
    snapshot(&f, stable);
    CHECK(memcmp(full, stable, sizeof full) == 0);

    fixture_close(&f);
}

int main(void) {
    test_steady_frame_replays();
    test_leaf_change_rerecords_sibling_replays();
    test_scale_change_invalidates_all_records();
    test_scroll_offset_invalidates_descendant_records();
    test_hidpi_scroll_clip_alignment();
    test_child_removal_invalidates_record();
    return TEST_REPORT();
}
