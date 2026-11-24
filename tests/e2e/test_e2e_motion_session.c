/*
 * e2e_motion_session — public-API multi-frame animation E2E (motion + ui + svg + gfx CPU).
 *
 * Drives a realistic animated application through <optics/optics.h> only:
 *   - optics_motion_spring animates a slider and indicator bar from 0.0 -> 1.0;
 *   - optics_motion_hysteresis latches a binary status badge once past threshold;
 *   - optics_ui builds each frame, publishes an immutable snapshot, and replays
 *     to CPU Canvas pixels;
 *   - verifies monotonic intermediate motion, distinct start/mid/end pixels,
 *     exact convergence to settled rest, and that reduced_motion=true reaches
 *     byte-identical final pixels in a single frame.
 */

#include "optics_test.h"
#include "ui_test.h"

#include <optics/gfx/canvas.h>
#include <optics/gfx/canvas_cpu.h>
#include <optics/optics.h>

#include <stdlib.h>
#include <string.h>

#define W 320u
#define H 160u
#define BYTES (W * H * 4u)

static size_t render_ui_frame(optics_ui *ui, float progress, bool active_badge, uint8_t *out_px) {
    optics_ui_input input = {
        .display_size = {(float)W, (float)H},
        .dt_seconds = 1.0f / 60.0f,
    };
    optics_ui_begin(ui, &input);
    optics_ui_label(ui, &(optics_ui_label_opts){
                            .text = active_badge ? "Motion: Settled" : "Motion: In Flight",
                        });
    float val = progress;
    optics_ui_slider(ui, &(optics_ui_slider_opts){
                             .box = {.id = "spring_slider", .width = 260.0f},
                             .label = "Progress",
                             .value = &val,
                             .min = 0.0f,
                             .max = 1.0f,
                         });
    optics_ui_end(ui);
    OT_EXPECT(!optics_ui_overflowed(ui));

    optics_ui_scene_snapshot *snap = nullptr;
    OT_REQUIRE(optics_ui_snapshot_create(ui, &snap) == OPTICS_GFX_OK);
    OT_REQUIRE(optics_ui_snapshot_activate(ui, snap) == OPTICS_GFX_OK);

    optics_gfx_canvas *canvas = nullptr;
    optics_gfx_canvas_desc cd = OPTICS_GFX_INIT(
        canvas_desc, .backend = OPTICS_GFX_CANVAS_BACKEND_CPU, .width = W, .height = H);
    OT_REQUIRE(optics_gfx_canvas_create(&cd, &canvas) == OPTICS_GFX_OK);

    optics_gfx_color clear = optics_gfx_color_rgba_premul(12, 14, 18, 255);
    OT_REQUIRE(optics_gfx_canvas_begin(
                   canvas, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                          .clear_color = &clear}) == OPTICS_GFX_OK);
    OT_EXPECT(optics_ui_snapshot_submit(snap, canvas) == OPTICS_GFX_OK);
    OT_REQUIRE(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);

    uint32_t rw = 0, rh = 0, stride = 0;
    const uint8_t *px = optics_gfx_canvas_read_pixels(canvas, &rw, &rh, &stride);
    OT_REQUIRE(px != nullptr && rw == W && rh == H);
    size_t lit = 0;
    for (uint32_t y = 0; y < H; ++y) {
        memcpy(out_px + (size_t)y * W * 4u, px + (size_t)y * stride, W * 4u);
        for (uint32_t x = 0; x < W; ++x) {
            const uint8_t *p = px + (size_t)y * stride + (size_t)x * 4u;
            if ((int)p[0] + (int)p[1] + (int)p[2] > 60)
                lit++;
        }
    }

    optics_gfx_canvas_release(canvas);
    optics_ui_snapshot_release(snap);
    return lit;
}

static size_t count_diff(const uint8_t *a, const uint8_t *b) {
    size_t diff = 0;
    for (uint32_t i = 0; i < W * H; ++i) {
        const uint8_t *pa = a + (size_t)i * 4u;
        const uint8_t *pb = b + (size_t)i * 4u;
        if (abs((int)pa[0] - (int)pb[0]) + abs((int)pa[1] - (int)pb[1]) +
                abs((int)pa[2] - (int)pb[2]) >
            12)
            diff++;
    }
    return diff;
}

int main(void) {
    optics_ui *ui_anim = nullptr;
    optics_ui *ui_reduced = nullptr;
    OT_REQUIRE(optics_ui_create(&(optics_ui_desc){.theme = optics_ui_theme_dark()}, &ui_anim) ==
               OPTICS_GFX_OK);
    OT_REQUIRE(optics_ui_create(&(optics_ui_desc){.theme = optics_ui_theme_dark()}, &ui_reduced) ==
               OPTICS_GFX_OK);

    uint8_t *px_start = malloc(BYTES);
    uint8_t *px_mid = malloc(BYTES);
    uint8_t *px_end = malloc(BYTES);
    uint8_t *px_reduced = malloc(BYTES);
    OT_REQUIRE(px_start && px_mid && px_end && px_reduced);

    /* Critically-damped spring: smooth monotonic approach with no overshoot. */
    optics_motion_spring spring = optics_motion_spring_at(0.0f);
    optics_motion_spring_params params = {.stiffness = 320.0f, .damping = 1.0f};
    optics_motion_hysteresis badge = optics_motion_hysteresis_init(false);

    /* Warm layout frame at value = 0.0. */
    (void)render_ui_frame(ui_anim, 0.0f, false, px_start);
    size_t lit_start = render_ui_frame(ui_anim, 0.0f, false, px_start);
    OT_EXPECT(lit_start > 500);

    float prev_val = 0.0f;
    bool captured_mid = false;
    bool settled = false;
    for (int f = 1; f <= 90; ++f) {
        float dt = 1.0f / 60.0f;
        float v = optics_motion_spring_advance(&spring, 1.0f, params, dt, false);
        OT_EXPECT(v >= prev_val - 1e-5f);
        OT_EXPECT(v <= 1.0001f);
        prev_val = v;

        bool high = optics_motion_hysteresis_step(&badge, false, 0.65f, 0.85f, v, 0.05f, dt, false);
        if (f == 8) {
            OT_EXPECT(v > 0.2f && v < 0.9f);
            (void)render_ui_frame(ui_anim, v, high, px_mid);
            captured_mid = true;
        }
        if (optics_motion_spring_settled(&spring, 1.0f, 1e-3f, 1e-2f) && high) {
            settled = true;
            break;
        }
    }
    OT_EXPECT(captured_mid);
    OT_EXPECT(settled);
    OT_EXPECT(badge.high == true);

    /* Snap settled frame to exact 1.0 and render twice so layout/animations settle. */
    (void)optics_motion_spring_snap_to(&spring, 1.0f);
    (void)render_ui_frame(ui_anim, spring.value, badge.high, px_end);
    (void)render_ui_frame(ui_anim, spring.value, badge.high, px_end);

    /* Start, mid, and settled frames must be visually distinct. */
    OT_EXPECT(count_diff(px_start, px_mid) > 30);
    OT_EXPECT(count_diff(px_mid, px_end) > 30);
    OT_EXPECT(count_diff(px_start, px_end) > 60);

    /* Reduced-motion path resolves spring & hysteresis in a single step and
     * renders byte-identically to the settled animated UI. */
    optics_motion_spring r_spring = optics_motion_spring_at(0.0f);
    optics_motion_hysteresis r_badge = optics_motion_hysteresis_init(false);
    float rv = optics_motion_spring_advance(&r_spring, 1.0f, params, 1.0f / 60.0f, true);
    bool rhigh =
        optics_motion_hysteresis_step(&r_badge, false, 0.65f, 0.85f, rv, 0.05f, 1.0f / 60.0f, true);
    OT_EXPECT_NEAR(rv, 1.0f, 1e-6);
    OT_EXPECT(rhigh == true);

    (void)render_ui_frame(ui_reduced, rv, rhigh, px_reduced);
    (void)render_ui_frame(ui_reduced, rv, rhigh, px_reduced);
    OT_EXPECT_MEM_EQ(px_reduced, px_end, BYTES);

    free(px_start);
    free(px_mid);
    free(px_end);
    free(px_reduced);
    optics_ui_release(ui_reduced);
    optics_ui_release(ui_anim);
    return OT_REPORT();
}
