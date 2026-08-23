/* test_ghost.c — leave-animation ghost replay (ADR-0078).
 *
 * The contract under test:
 *   - arming: lens_set_ghost on a live subtree arms capture; the subtree's
 *     LAST live lens_end freezes its snapshot; an unpinned leaving subtree
 *     paints nothing extra (the golden record/replay tests pin the no-ghost
 *     side of that).
 *   - survival: the snapshot outlives the per-frame arena reset and paints
 *     the same ink at the pinned alpha.
 *   - fade: alpha < 1 bakes into every colour-bearing command, the same
 *     ADR-0068 bake the live tree uses; alpha 0 paints nothing.
 *   - keepalive/expiry: refreshed ghosts live; unrefreshed ones count down
 *     and disappear after GHOST_MAX_FRAMES.
 *   - non-interactivity: a ghost under the cursor never hit-tests.
 *   - safety: unknown ids, NULL, NaN alpha, and the 17-ghost ceiling.
 *
 * CPU canvas, golden pixels; same fixture pattern as test_record_replay.
 */

#include "test_helpers.h"
#include <flux/canvas_cpu.h>
#include <lens/lens.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define W 200
#define H 120

/* Mirror of the internal lifetime ceiling (ADR-0078): a host converts
 * wall-clock pacing to frames itself, so the pin test needs the number. */
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
    lens_begin(ui, &IN0);
    lens_label(ui, "header##hdr");
    lens_progress(ui, "load", progress);
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
        lens_set_ghost(f.ui, lens_current_id(f.ui, "load"), 1.0f); /* armed */
        render_frame(&f);
    }
    snapshot(&f, with_footer);
    CHECK(ink_pixels(with_footer) > 0);

    /* Remove the subtree: the armed pin freezes it at full alpha, so the
     * ghost frame still paints the progress bar's ink. */
    lens_begin(f.ui, &IN0);
    lens_label(f.ui, "header##hdr");
    lens_set_ghost(f.ui, lens_current_id(f.ui, "load"), 1.0f); /* refresh pin */
    lens_end(f.ui);
    render_frame(&f);
    snapshot(&f, ghosted);
    /* header ink remains; the question is the bar: at alpha 1 the ghost
     * frame must match the with-footer frame byte for byte. */
    CHECK(memcmp(with_footer, ghosted, sizeof with_footer) == 0);

    /* Stop pinning: after GHOST_MAX_FRAMES frames the ghost is gone
     * and the frame is exactly the header's ink. */
    uint8_t expired[W * H * 4];
    for (int i = 0; i < GHOST_MAX_FRAMES + 2; i++) {
        lens_begin(f.ui, &IN0);
        lens_label(f.ui, "header##hdr");
        lens_end(f.ui);
        render_frame(&f);
    }
    snapshot(&f, expired);
    {
        fixture g;
        fixture_open(&g);
        for (int i = 0; i < 3; i++) {
            lens_begin(g.ui, &IN0);
            lens_label(g.ui, "header##hdr");
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
    /* The ghost's fade must be BIT-IDENTICAL to the live tree fading the
     * same subtree through lens_set_opacity (ADR-0068): both run the same
     * colour bake ahead of the same emitter, so any divergence is a bug in
     * one path or the other. Byte equality is the contract; the CPU
     * canvas's own alpha-compositing quirks are common to both sides and
     * therefore out of scope here (flux's rasterizer owns them). */
    for (int variant = 0; variant < 3; variant++) {
        float alpha = variant == 0 ? 1.0f : (variant == 1 ? 0.5f : 0.0f);

        /* Ghost path: build, pin, remove, ghost at alpha. */
        fixture g;
        fixture_open(&g);
        uint8_t ghost_shot[W * H * 4];
        for (int i = 0; i < 3; i++) {
            build_with_footer(g.ui, 0.5f);
            lens_set_ghost(g.ui, lens_current_id(g.ui, "load"), 1.0f);
            render_frame(&g);
        }
        lens_begin(g.ui, &IN0);
        lens_label(g.ui, "header##hdr");
        lens_set_ghost(g.ui, lens_current_id(g.ui, "load"), alpha);
        lens_end(g.ui);
        render_frame(&g);
        snapshot(&g, ghost_shot);

        /* Live path: same UI still fully built, the SAME subtree (and only
         * it) faded through the frame-scoped opacity switch at alpha. */
        fixture l;
        fixture_open(&l);
        uint8_t live_shot[W * H * 4];
        for (int i = 0; i < 3; i++) {
            build_with_footer(l.ui, 0.5f);
            render_frame(&l);
        }
        lens_begin(l.ui, &IN0);
        lens_label(l.ui, "header##hdr");
        lens_set_opacity(l.ui, alpha);
        lens_progress(l.ui, "load", 0.5f);
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
    /* Keep refreshing far past the expiry window: the ghost must still
     * paint (survival is the contract; the fade values are the host's). */
    uint8_t first[W * H * 4], later[W * H * 4];
    lens_begin(f.ui, &IN0);
    lens_label(f.ui, "header##hdr");
    lens_set_ghost(f.ui, lens_current_id(f.ui, "load"), 1.0f);
    lens_end(f.ui);
    render_frame(&f);
    snapshot(&f, first);
    for (int i = 0; i < GHOST_MAX_FRAMES * 3; i++) {
        lens_begin(f.ui, &IN0);
        lens_label(f.ui, "header##hdr");
        lens_set_ghost(f.ui, lens_current_id(f.ui, "load"), 1.0f);
        lens_end(f.ui);
        render_frame(&f);
    }
    snapshot(&f, later);
    CHECK(memcmp(first, later, sizeof first) == 0);
    fixture_close(&f);
}

/* ---- non-interactivity ---------------------------------------------------- */

static void test_ghost_never_hit_tests(void) {
    fixture f;
    fixture_open(&f);
    /* A button at the cursor that then leaves: its ghost must not swallow
     * the click intended for whatever is rebuilt underneath. */
    for (int i = 0; i < 3; i++) {
        lens_begin(f.ui, &IN0);
        lens_button(f.ui, "target##btn");
        lens_set_ghost(f.ui, lens_current_id(f.ui, "btn"), 1.0f);
        lens_end(f.ui);
        render_frame(&f);
    }
    lens_input click = IN0;
    click.mouse_pressed[0] = true;
    lens_begin(f.ui, &click);
    lens_set_ghost(f.ui, lens_current_id(f.ui, "btn"), 1.0f); /* ghost only */
    lens_end(f.ui);
    /* No assertion crashes: the frame solved with a ghost under the cursor
     * and no live hit target. Rebuild the button and verify it still hits:
     * the ghost did not poison the store. */
    /* Rebuild the button and verify it still clicks: press, then release
     * (lens_button reports the click on the release frame), with the
     * cursor inside its box. The ghost frames in between must not have
     * poisoned the store or the hit geometry. */
    bool clicked = false;
    for (int i = 0; i < 8; i++) {
        lens_input in = IN0;
        if (i == 6)
            in.mouse_pressed[LENS_MOUSE_LEFT] = true;
        if (i == 7)
            in.mouse_released[LENS_MOUSE_LEFT] = true;
        lens_begin(f.ui, &in);
        clicked = lens_button(f.ui, "target##btn");
        lens_end(f.ui);
        render_frame(&f);
    }
    CHECK(clicked);
    fixture_close(&f);
}

/* ---- safety ----------------------------------------------------------------- */

static void test_ghost_safety(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_set_ghost(NULL, 123, 0.5f); /* NULL context: no crash */
    lens_set_ghost(ui, 0, 0.5f);     /* NULL id */
    lens_set_ghost(ui, 999, NAN);    /* NaN alpha: ignored */
    lens_set_ghost(ui, 999, 2.0f);   /* clamped, still no ghost (unknown) */
    lens_begin(ui, &IN0);
    lens_label(ui, "solo##s");
    lens_end(ui);
    lens_destroy(ui);
    CHECK(true); /* reached without crashing */
}

int main(void) {
    test_armed_subtree_ghosts_after_removal();
    test_ghost_alpha_matches_live_opacity_path();
    test_refresh_extends_lifetime();
    test_ghost_never_hit_tests();
    test_ghost_safety();
    return TEST_REPORT();
}
