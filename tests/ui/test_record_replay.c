/* test_record_replay.c — subtree display-list record/replay (ADR-0030). */

#include "lens_test.h"
#include "optics_test.h"
#include <optics/gfx/canvas_cpu.h>
#include <optics/ui.h>
#include <stdio.h>
#include <string.h>

#define W 400
#define H 300

static const optics_ui_input IN0 = {.display_size = {W, H}, .dt_seconds = 0.016f};

typedef struct {
    optics_ui *ui;
    optics_gfx_canvas *canvas;
} fixture;

static void fixture_open(fixture *f) {
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &f->ui) == OPTICS_GFX_OK);
    OT_EXPECT(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = W,
                                                           .height = H},
                                 &f->canvas) == OPTICS_GFX_OK);
}

static void fixture_close(fixture *f) {
    optics_gfx_canvas_release(f->canvas);
    optics_ui_release(f->ui);
}

static void render_frame(fixture *f) {
    optics_gfx_color clear = optics_gfx_color_rgba_premul(0, 0, 0, 255);
    OT_EXPECT(optics_gfx_canvas_begin(f->canvas, &(optics_gfx_canvas_pass_desc){
                                                     .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                     .clear_color = &clear}) == OPTICS_GFX_OK);
    OT_EXPECT(optics_ui_test_snapshot_render(f->ui, f->canvas) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_end(f->canvas) == OPTICS_GFX_OK);
}

static void snapshot(fixture *f, uint8_t *out) {
    uint32_t pw = 0, ph = 0, stride = 0;
    const uint8_t *fb = optics_gfx_canvas_cpu_pixels(f->canvas, &pw, &ph, &stride);
    OT_EXPECT(fb != nullptr && pw == W && ph == H && stride == W * 4);
    memcpy(out, fb, (size_t)W * H * 4);
}

/* Two sibling blocks so a change in one leaves the other replayable. */
static void build_two_blocks(optics_ui *ui, const optics_ui_input *in, float value) {
    char val_str[32];
    snprintf(val_str, sizeof val_str, "val=%.2f", (double)value);
    optics_ui_begin(ui, in);
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "header##hdr"});
    optics_ui_label(ui, &(optics_ui_label_opts){.text = val_str, .box = {.id = "val_node"}});
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "footer##ftr"});
    optics_ui_test_end(ui);
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

    /* Frame 1: nothing changed -> replay path */
    build_two_blocks(f.ui, &IN0, 0.5f);
    render_frame(&f);
    snapshot(&f, frame1);
    OT_EXPECT(memcmp(base, frame1, sizeof base) == 0);

    /* Frame 2: still unchanged -> still replaying identical pixels */
    build_two_blocks(f.ui, &IN0, 0.5f);
    render_frame(&f);
    snapshot(&f, frame2);
    OT_EXPECT(memcmp(base, frame2, sizeof base) == 0);

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
    build_two_blocks(f.ui, &IN0, 0.75f);
    render_frame(&f);
    snapshot(&f, changed);
    OT_EXPECT(memcmp(base, changed, sizeof base) != 0); /* pixels changed */

    /* Settle on the new value: replays the newly-recorded state */
    build_two_blocks(f.ui, &IN0, 0.75f);
    render_frame(&f);
    snapshot(&f, settled);
    OT_EXPECT(memcmp(changed, settled, sizeof changed) == 0);

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

    optics_ui_set_scale(f.ui, 2.0f);
    build_two_blocks(f.ui, &IN0, 0.5f);
    render_frame(&f);

    fixture_close(&f);
}

/* Scroll / viewport-translation changes must invalidate the recorded
 * display lists inside the scroll container. */
static void test_scroll_offset_invalidates_descendant_records(void) {
    fixture f;
    fixture_open(&f);

    /* Build a scroll container with several items */
    optics_ui_input in_init = IN0;
    in_init.cursor = (optics_gfx_point){50, 50};
    for (int i = 0; i < 3; i++) {
        optics_ui_begin(f.ui, &in_init);
        optics_ui_size(f.ui, W, 100);
        optics_ui_scroll_begin(f.ui, &(optics_ui_scroll_opts){.box = {.id = "scroll"}});
        for (int j = 0; j < 15; j++) {
            char lbl[32];
            snprintf(lbl, sizeof lbl, "item %d", j);
            optics_ui_size(f.ui, 0, 30);
            optics_ui_label(f.ui, &(optics_ui_label_opts){.text = lbl});
        }
        optics_ui_scroll_end(f.ui);
        optics_ui_test_end(f.ui);
        render_frame(&f);
    }

    /* Scrolling moves the translation: records inside must re-record */
    optics_ui_input in_scrolled = in_init;
    in_scrolled.scroll_y = -3.0f;
    optics_ui_begin(f.ui, &in_scrolled);
    optics_ui_size(f.ui, W, 100);
    optics_ui_scroll_begin(f.ui, &(optics_ui_scroll_opts){.box = {.id = "scroll"}});
    for (int j = 0; j < 15; j++) {
        char lbl[32];
        snprintf(lbl, sizeof lbl, "item %d", j);
        optics_ui_size(f.ui, 0, 30);
        optics_ui_label(f.ui, &(optics_ui_label_opts){.text = lbl});
    }
    optics_ui_scroll_end(f.ui);
    optics_ui_test_end(f.ui);
    render_frame(&f);

    fixture_close(&f);
}

static void test_hidpi_scroll_clip_alignment(void) {
    fixture f;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &f.ui) == OPTICS_GFX_OK);
    OT_EXPECT(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 2.0f,
                                                           .width = W * 2,
                                                           .height = H * 2},
                                 &f.canvas) == OPTICS_GFX_OK);
    optics_ui_set_scale(f.ui, 2.0f);

    const optics_ui_input hidpi_in = {
        .display_size = {W, H}, .cursor = {50, 50}, .dt_seconds = 0.016f};

    optics_ui_begin(f.ui, &hidpi_in);
    optics_ui_size(f.ui, W, H);
    optics_ui_scroll_begin(f.ui, &(optics_ui_scroll_opts){.box = {.id = "hidpi_scroll"}});
    optics_ui_label(f.ui, &(optics_ui_label_opts){.text = "Visible Label"});
    optics_ui_scroll_end(f.ui);
    optics_ui_test_end(f.ui);

    optics_gfx_color clear = optics_gfx_color_rgba_premul(0, 0, 0, 255);
    OT_EXPECT(optics_gfx_canvas_begin(f.canvas, &(optics_gfx_canvas_pass_desc){
                                                    .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                    .clear_color = &clear}) == OPTICS_GFX_OK);
    OT_EXPECT(optics_ui_test_snapshot_render(f.ui, f.canvas) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_end(f.canvas) == OPTICS_GFX_OK);

    uint32_t pw = 0, ph = 0, stride = 0;
    const uint8_t *fb = optics_gfx_canvas_cpu_pixels(f.canvas, &pw, &ph, &stride);
    OT_EXPECT(fb != nullptr && pw == W * 2 && ph == H * 2);

    bool found_non_zero = false;
    for (size_t i = 0; i < (size_t)ph * stride; i++) {
        if (fb[i] > 0) {
            found_non_zero = true;
            break;
        }
    }
    OT_EXPECT(found_non_zero == true);

    optics_gfx_canvas_release(f.canvas);
    optics_ui_release(f.ui);
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

    optics_ui_begin(f.ui, &IN0);
    optics_ui_label(f.ui, &(optics_ui_label_opts){.text = "header##hdr"});
    optics_ui_label(f.ui, &(optics_ui_label_opts){.text = "val=0.50", .box = {.id = "val_node"}});
    optics_ui_test_end(f.ui);
    render_frame(&f);
    snapshot(&f, shrunk);
    OT_EXPECT(memcmp(full, shrunk, sizeof full) != 0);

    {
        fixture g;
        fixture_open(&g);
        for (int i = 0; i < 3; i++) {
            optics_ui_begin(g.ui, &IN0);
            optics_ui_label(g.ui, &(optics_ui_label_opts){.text = "header##hdr"});
            optics_ui_label(g.ui,
                            &(optics_ui_label_opts){.text = "val=0.50", .box = {.id = "val_node"}});
            optics_ui_test_end(g.ui);
            render_frame(&g);
        }
        snapshot(&g, reference);
        fixture_close(&g);
    }
    OT_EXPECT(memcmp(shrunk, reference, sizeof shrunk) == 0);

    optics_ui_begin(f.ui, &IN0);
    optics_ui_test_end(f.ui);
    render_frame(&f);
    snapshot(&f, stable);
    bool any_ink = false;
    for (size_t i = 0; i + 2 < sizeof stable; i += 4) {
        if (stable[i] || stable[i + 1] || stable[i + 2]) {
            any_ink = true;
            break;
        }
    }
    OT_EXPECT(!any_ink);

    for (int i = 0; i < 3; i++) {
        build_two_blocks(f.ui, &IN0, 0.5f);
        render_frame(&f);
    }
    snapshot(&f, stable);
    OT_EXPECT(memcmp(full, stable, sizeof full) == 0);

    fixture_close(&f);
}

int main(void) {
    test_steady_frame_replays();
    test_leaf_change_rerecords_sibling_replays();
    test_scale_change_invalidates_all_records();
    test_scroll_offset_invalidates_descendant_records();
    test_hidpi_scroll_clip_alignment();
    test_child_removal_invalidates_record();
    return OT_REPORT();
}
