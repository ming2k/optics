/* test_ghost.c — leave-animation ghost replay (ADR-0078). */

#include "optics_test.h"
#include "ui_test.h"
#include <math.h>
#include <optics/gfx/canvas_cpu.h>
#include <optics/ui.h>
#include <stdio.h>
#include <string.h>

#define W 200
#define H 120

#define GHOST_MAX_FRAMES 64
static const optics_ui_input IN0 = {
    .display_size = {W, H}, .cursor = {50, 20}, .dt_seconds = 0.016f};

typedef struct fixture {
    optics_ui *ui;
    optics_gfx_canvas *canvas;
} fixture;

static void fixture_open(fixture *f) {
    f->ui = nullptr;
    f->canvas = nullptr;
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

static void snapshot(const fixture *f, uint8_t *out) {
    uint32_t w = 0, h = 0, stride = 0;
    const uint8_t *fb = optics_gfx_canvas_cpu_pixels(f->canvas, &w, &h, &stride);
    OT_EXPECT(fb != nullptr && w == W && h == H && stride == W * 4);
    memcpy(out, fb, (size_t)h * stride);
}

static void render_frame(const fixture *f) {
    optics_gfx_color clear = optics_gfx_color_rgba_premul(0, 0, 0, 255);
    OT_EXPECT(optics_gfx_canvas_begin(f->canvas, &(optics_gfx_canvas_pass_desc){
                                                     .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                     .clear_color = &clear}) == OPTICS_GFX_OK);
    OT_EXPECT(optics_ui_test_snapshot_render(f->ui, f->canvas) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_end(f->canvas) == OPTICS_GFX_OK);
}

static void build_with_footer(optics_ui *ui, float progress) {
    char val_str[32];
    snprintf(val_str, sizeof val_str, "val=%.2f", (double)progress);
    optics_ui_begin(ui, &IN0);
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "header##hdr"});
    optics_ui_label(ui, &(optics_ui_label_opts){.text = val_str, .box = {.id = "load"}});
    optics_ui_set_ghost(ui, &(optics_ui_set_ghost_opts){
                                .subtree_root = optics_ui_current_id(ui, "load"), .alpha = 1.0f});
    optics_ui_test_end(ui);
}

static size_t ink_pixels(const uint8_t *fb) {
    size_t count = 0;
    for (size_t i = 0; i + 2 < (size_t)W * H * 4; i += 4)
        if (fb[i] || fb[i + 1] || fb[i + 2])
            count++;
    return count;
}

/* ---- arming + capture + survival ------------------------------------- */

static void test_armed_subtree_ghosts_after_removal(void) {
    fixture f;
    fixture_open(&f);
    uint8_t with_footer[W * H * 4], ghosted[W * H * 4];

    for (int i = 0; i < 3; i++) {
        build_with_footer(f.ui, 0.5f);
        render_frame(&f);
    }
    snapshot(&f, with_footer);
    OT_EXPECT(ink_pixels(with_footer) > 0);

    /* Remove the subtree: the armed pin freezes it at full alpha */
    optics_ui_begin(f.ui, &IN0);
    optics_ui_label(f.ui, &(optics_ui_label_opts){.text = "header##hdr"});
    optics_ui_set_ghost(
        f.ui, &(optics_ui_set_ghost_opts){.subtree_root = optics_ui_current_id(f.ui, "load"),
                                          .alpha = 1.0f}); /* refresh pin */
    optics_ui_test_end(f.ui);
    render_frame(&f);
    snapshot(&f, ghosted);
    OT_EXPECT(memcmp(with_footer, ghosted, sizeof with_footer) == 0);

    /* Stop pinning: after GHOST_MAX_FRAMES frames the ghost is gone
     * and the frame is exactly the header's ink. */
    uint8_t expired[W * H * 4];
    for (int i = 0; i < GHOST_MAX_FRAMES + 10; i++) {
        optics_ui_begin(f.ui, &IN0);
        optics_ui_label(f.ui, &(optics_ui_label_opts){.text = "header##hdr"});
        optics_ui_test_end(f.ui);
        render_frame(&f);
    }
    snapshot(&f, expired);
    {
        fixture g;
        fixture_open(&g);
        for (int i = 0; i < 3; i++) {
            optics_ui_begin(g.ui, &IN0);
            optics_ui_label(g.ui, &(optics_ui_label_opts){.text = "header##hdr"});
            optics_ui_test_end(g.ui);
            render_frame(&g);
        }
        uint8_t reference[W * H * 4];
        snapshot(&g, reference);
        OT_EXPECT(memcmp(expired, reference, sizeof expired) == 0);
        fixture_close(&g);
    }
    fixture_close(&f);
}

/* ---- fade bake --------------------------------------------------------- */

static void test_ghost_alpha_matches_live_opacity_path(void) {
    for (int variant = 0; variant < 3; variant++) {
        float alpha = variant == 0 ? 1.0f : (variant == 1 ? 0.5f : 0.0f);

        /* Ghost path */
        fixture g;
        fixture_open(&g);
        uint8_t ghost_shot[W * H * 4];
        for (int i = 0; i < 3; i++) {
            build_with_footer(g.ui, 0.5f);
            render_frame(&g);
        }
        optics_ui_begin(g.ui, &IN0);
        optics_ui_label(g.ui, &(optics_ui_label_opts){.text = "header##hdr"});
        optics_ui_set_ghost(
            g.ui, &(optics_ui_set_ghost_opts){.subtree_root = optics_ui_current_id(g.ui, "load"),
                                              .alpha = alpha});
        optics_ui_test_end(g.ui);
        render_frame(&g);
        snapshot(&g, ghost_shot);

        /* Live path: same alpha on the live tree */
        fixture l;
        fixture_open(&l);
        uint8_t live_shot[W * H * 4];
        for (int i = 0; i < 3; i++) {
            optics_ui_begin(l.ui, &IN0);
            optics_ui_label(l.ui, &(optics_ui_label_opts){.text = "header##hdr"});
            optics_ui_label(l.ui,
                            &(optics_ui_label_opts){.text = "val=0.50", .box = {.id = "load"}});
            optics_ui_test_end(l.ui);
            render_frame(&l);
        }
        optics_ui_begin(l.ui, &IN0);
        optics_ui_label(l.ui, &(optics_ui_label_opts){.text = "header##hdr"});
        optics_ui_set_opacity(l.ui, alpha);
        optics_ui_label(l.ui, &(optics_ui_label_opts){.text = "val=0.50", .box = {.id = "load"}});
        optics_ui_set_opacity(l.ui, 1.0f);
        optics_ui_test_end(l.ui);
        render_frame(&l);
        snapshot(&l, live_shot);

        OT_EXPECT(memcmp(ghost_shot, live_shot, sizeof ghost_shot) == 0);
        fixture_close(&g);
        fixture_close(&l);
    }
}

/* ---- refresh keepalive -------------------------------------------------- */

static void test_refresh_extends_lifetime(void) {
    fixture f;
    fixture_open(&f);
    for (int i = 0; i < 3; i++) {
        build_with_footer(f.ui, 0.5f);
        optics_ui_set_ghost(
            f.ui, &(optics_ui_set_ghost_opts){.subtree_root = optics_ui_current_id(f.ui, "load"),
                                              .alpha = 1.0f});
        render_frame(&f);
    }
    uint8_t first[W * H * 4], later[W * H * 4];

    /* frame 1 ghost */
    optics_ui_begin(f.ui, &IN0);
    optics_ui_label(f.ui, &(optics_ui_label_opts){.text = "header##hdr"});
    optics_ui_set_ghost(
        f.ui, &(optics_ui_set_ghost_opts){.subtree_root = optics_ui_current_id(f.ui, "load"),
                                          .alpha = 1.0f});
    optics_ui_test_end(f.ui);
    render_frame(&f);
    snapshot(&f, first);

    /* 20 frames later, refreshed every frame -> still looks identical */
    for (int i = 0; i < 20; i++) {
        optics_ui_begin(f.ui, &IN0);
        optics_ui_label(f.ui, &(optics_ui_label_opts){.text = "header##hdr"});
        optics_ui_set_ghost(
            f.ui, &(optics_ui_set_ghost_opts){.subtree_root = optics_ui_current_id(f.ui, "load"),
                                              .alpha = 1.0f});
        optics_ui_test_end(f.ui);
        render_frame(&f);
    }
    snapshot(&f, later);
    OT_EXPECT(memcmp(first, later, sizeof first) == 0);

    fixture_close(&f);
}

/* ---- non-interactive invariant ------------------------------------------ */

static void test_ghost_never_hit_tests(void) {
    fixture f;
    fixture_open(&f);

    for (int i = 0; i < 3; i++) {
        optics_ui_begin(f.ui, &IN0);
        optics_ui_button(f.ui, &(optics_ui_button_opts){.label = "target##btn"});
        optics_ui_set_ghost(
            f.ui, &(optics_ui_set_ghost_opts){
                      .subtree_root = optics_ui_current_id(f.ui, "target##btn"), .alpha = 1.0f});
        optics_ui_test_end(f.ui);
        render_frame(&f);
    }
    optics_ui_input click = IN0;
    click.mouse_pressed[0] = true;
    optics_ui_begin(f.ui, &click);
    optics_ui_set_ghost(
        f.ui, &(optics_ui_set_ghost_opts){.subtree_root = optics_ui_current_id(f.ui, "target##btn"),
                                          .alpha = 1.0f});
    optics_ui_test_end(f.ui);

    bool clicked = false;
    for (int i = 0; i < 8; i++) {
        optics_ui_input in = IN0;
        if (i == 6)
            in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
        if (i == 7)
            in.mouse_released[OPTICS_UI_MOUSE_LEFT] = true;
        optics_ui_begin(f.ui, &in);
        clicked = optics_ui_button(f.ui, &(optics_ui_button_opts){.label = "target##btn"}).clicked;
        optics_ui_test_end(f.ui);
        render_frame(&f);
    }
    OT_EXPECT(clicked);
    fixture_close(&f);
}

int main(void) {
    test_armed_subtree_ghosts_after_removal();
    test_ghost_alpha_matches_live_opacity_path();
    test_refresh_extends_lifetime();
    test_ghost_never_hit_tests();
    return OT_REPORT();
}
