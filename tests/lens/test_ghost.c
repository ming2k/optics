/* test_ghost.c — leave-animation ghost replay (ADR-0078). */

#include "test_helpers.h"
#include <flux/canvas_cpu.h>
#include <lens/lens.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define W 200
#define H 120

#define GHOST_MAX_FRAMES 64
static const lens_input IN0 = {.display_size = {W, H}, .cursor = {50, 20}, .dt_seconds = 0.016f};

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

static void build_with_footer(lens *ui, float progress) {
    char val_str[32];
    snprintf(val_str, sizeof val_str, "val=%.2f", (double)progress);
    lens_begin(ui, &IN0);
    lens_label(ui, &(lens_label_opts){.text = "header##hdr"});
    lens_label(ui, &(lens_label_opts){.text = val_str, .box = {.id = "load"}});
    lens_set_ghost(ui, lens_current_id(ui, "load"), 1.0f);
    lens_end(ui);
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
    CHECK(ink_pixels(with_footer) > 0);

    /* Remove the subtree: the armed pin freezes it at full alpha */
    lens_begin(f.ui, &IN0);
    lens_label(f.ui, &(lens_label_opts){.text = "header##hdr"});
    lens_set_ghost(f.ui, lens_current_id(f.ui, "load"), 1.0f); /* refresh pin */
    lens_end(f.ui);
    render_frame(&f);
    snapshot(&f, ghosted);
    CHECK(memcmp(with_footer, ghosted, sizeof with_footer) == 0);

    /* Stop pinning: after GHOST_MAX_FRAMES frames the ghost is gone
     * and the frame is exactly the header's ink. */
    uint8_t expired[W * H * 4];
    for (int i = 0; i < GHOST_MAX_FRAMES + 10; i++) {
        lens_begin(f.ui, &IN0);
        lens_label(f.ui, &(lens_label_opts){.text = "header##hdr"});
        lens_end(f.ui);
        render_frame(&f);
    }
    snapshot(&f, expired);
    {
        fixture g;
        fixture_open(&g);
        for (int i = 0; i < 3; i++) {
            lens_begin(g.ui, &IN0);
            lens_label(g.ui, &(lens_label_opts){.text = "header##hdr"});
            lens_end(g.ui);
            render_frame(&g);
        }
        uint8_t reference[W * H * 4];
        snapshot(&g, reference);
        CHECK(memcmp(expired, reference, sizeof expired) == 0);
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
        lens_begin(g.ui, &IN0);
        lens_label(g.ui, &(lens_label_opts){.text = "header##hdr"});
        lens_set_ghost(g.ui, lens_current_id(g.ui, "load"), alpha);
        lens_end(g.ui);
        render_frame(&g);
        snapshot(&g, ghost_shot);

        /* Live path: same alpha on the live tree */
        fixture l;
        fixture_open(&l);
        uint8_t live_shot[W * H * 4];
        for (int i = 0; i < 3; i++) {
            lens_begin(l.ui, &IN0);
            lens_label(l.ui, &(lens_label_opts){.text = "header##hdr"});
            lens_label(l.ui, &(lens_label_opts){.text = "val=0.50", .box = {.id = "load"}});
            lens_end(l.ui);
            render_frame(&l);
        }
        lens_begin(l.ui, &IN0);
        lens_label(l.ui, &(lens_label_opts){.text = "header##hdr"});
        lens_set_opacity(l.ui, alpha);
        lens_label(l.ui, &(lens_label_opts){.text = "val=0.50", .box = {.id = "load"}});
        lens_set_opacity(l.ui, 1.0f);
        lens_end(l.ui);
        render_frame(&l);
        snapshot(&l, live_shot);

        CHECK(memcmp(ghost_shot, live_shot, sizeof ghost_shot) == 0);
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
        lens_set_ghost(f.ui, lens_current_id(f.ui, "load"), 1.0f);
        render_frame(&f);
    }
    uint8_t first[W * H * 4], later[W * H * 4];

    /* frame 1 ghost */
    lens_begin(f.ui, &IN0);
    lens_label(f.ui, &(lens_label_opts){.text = "header##hdr"});
    lens_set_ghost(f.ui, lens_current_id(f.ui, "load"), 1.0f);
    lens_end(f.ui);
    render_frame(&f);
    snapshot(&f, first);

    /* 20 frames later, refreshed every frame -> still looks identical */
    for (int i = 0; i < 20; i++) {
        lens_begin(f.ui, &IN0);
        lens_label(f.ui, &(lens_label_opts){.text = "header##hdr"});
        lens_set_ghost(f.ui, lens_current_id(f.ui, "load"), 1.0f);
        lens_end(f.ui);
        render_frame(&f);
    }
    snapshot(&f, later);
    CHECK(memcmp(first, later, sizeof first) == 0);

    fixture_close(&f);
}

/* ---- non-interactive invariant ------------------------------------------ */

static void test_ghost_never_hit_tests(void) {
    fixture f;
    fixture_open(&f);

    for (int i = 0; i < 3; i++) {
        lens_begin(f.ui, &IN0);
        lens_button(f.ui, &(lens_button_opts){.label = "target##btn"});
        lens_set_ghost(f.ui, lens_current_id(f.ui, "target##btn"), 1.0f);
        lens_end(f.ui);
        render_frame(&f);
    }
    lens_input click = IN0;
    click.mouse_pressed[0] = true;
    lens_begin(f.ui, &click);
    lens_set_ghost(f.ui, lens_current_id(f.ui, "target##btn"), 1.0f);
    lens_end(f.ui);

    bool clicked = false;
    for (int i = 0; i < 8; i++) {
        lens_input in = IN0;
        if (i == 6)
            in.mouse_pressed[LENS_MOUSE_LEFT] = true;
        if (i == 7)
            in.mouse_released[LENS_MOUSE_LEFT] = true;
        lens_begin(f.ui, &in);
        clicked = lens_button(f.ui, &(lens_button_opts){.label = "target##btn"}).clicked;
        lens_end(f.ui);
        render_frame(&f);
    }
    CHECK(clicked);
    fixture_close(&f);
}

int main(void) {
    test_armed_subtree_ghosts_after_removal();
    test_ghost_alpha_matches_live_opacity_path();
    test_refresh_extends_lifetime();
    test_ghost_never_hit_tests();
    return TEST_REPORT();
}
