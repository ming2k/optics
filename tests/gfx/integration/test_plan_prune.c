/*
 * Deferred pass plan (ADR-0147): a registered effect must (a) produce
 * identical pixels when its output is consumed, and (b) cost nothing when
 * nothing consumes it.
 *
 * The second half is the point of the mechanism and the reason this test
 * exists: before ADR-0147 a blur computed unconditionally, so "the output
 * was never sampled" was indistinguishable from "the output was sampled" by
 * every externally visible measure except GPU time. The plan now makes the
 * difference observable through optics_gfx_canvas_get_plan_stats, and this test
 * pins it.
 */
#include "gfx_test.h"
#include "optics_test.h"
#include <optics/gfx.h>
#include <optics/gfx/canvas.h>
#include <optics/gfx/effect.h>
#include <optics/gfx/observe.h>
#include <optics/gfx/vulkan.h>
#include <optics/material.h>

#include <string.h>

#define W 64u
#define H 64u
#define BYTES (W * H * 4u)

static void fill_edge_pattern(uint8_t *px) {
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            uint8_t v = (x < W / 2) ? 0u : 255u;
            px[(y * W + x) * 4 + 0] = v;
            px[(y * W + x) * 4 + 1] = v;
            px[(y * W + x) * 4 + 2] = v;
            px[(y * W + x) * 4 + 3] = 255u;
        }
    }
}

static void dummy_pass_record(void *params, VkCommandBuffer cmd) {
    (void)params;
    (void)cmd;
}

int main(void) {
    /* Request the pass-label capability: without it the label path is
     * legitimately suppressed and the final assertion cannot observe
     * anything. */
    static const optics_gfx_observe_opts observe = {
        .type = OPTICS_GFX_TYPE_OBSERVE_OPTS,
        .capabilities = OPTICS_GFX_OBSERVE_COUNTS | OPTICS_GFX_OBSERVE_PASS_LABELS,
        .log = optics_gfx_console_logger,
    };
    optics_gfx_device_desc dd = OPTICS_GFX_DEVICE_DESC_INIT;
    dd.headless = true;
    dd.next = &observe;
    optics_gfx_device *d = nullptr;
    if (optics_gfx_device_create(&dd, &d) != OPTICS_GFX_OK) {
        fprintf(stderr, "test_plan_prune: no Vulkan device; skipping\n");
        OT_SKIP("no Vulkan device");
    }

    optics_gfx_surface *s = nullptr;
    optics_gfx_surface_desc sdesc = OPTICS_GFX_SURFACE_DESC_INIT;
    sdesc.width = W;
    sdesc.height = H;
    OT_EXPECT(optics_gfx_surface_create(d, &sdesc, &s) == OPTICS_GFX_OK);

    optics_gfx_canvas *canvas = nullptr;
    OT_EXPECT(optics_gfx_canvas_create(
                  &(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC, .surface = s},
                  &canvas) == OPTICS_GFX_OK);

    uint8_t *px = calloc(1, BYTES);
    OT_EXPECT(px != nullptr);
    fill_edge_pattern(px);

    optics_gfx_image_desc idesc = OPTICS_GFX_IMAGE_DESC_INIT;
    idesc.width = W;
    idesc.height = H;
    idesc.format = OPTICS_GFX_FORMAT_RGBA8_UNORM;
    idesc.initial_data = px;
    optics_gfx_image *target = nullptr;
    OT_EXPECT(optics_gfx_image_create(d, &idesc, &target) == OPTICS_GFX_OK);

    optics_gfx_blur_filter *blur = nullptr;
    OT_EXPECT(optics_gfx_blur_filter_create(d, &blur) == OPTICS_GFX_OK);

    /* ---- 1. A deferred blur nothing samples is pruned ---- */
    {
        optics_gfx_frame *frame = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);

        optics_gfx_effect_blur_desc bd = OPTICS_GFX_EFFECT_BLUR_DESC_INIT;
        bd.input = target;
        bd.sigma = 6.0f;
        optics_gfx_image *blurred = nullptr;
        OT_EXPECT(optics_gfx_blur_filter_apply(
                      blur, &(optics_gfx_blur_filter_apply_opts){
                                .frame = frame, .desc = &bd, .out = &blurred}) == OPTICS_GFX_OK);
        OT_EXPECT(blurred != nullptr);

        /* Nothing samples `blurred`. Draw only the sharp capture, then
         * submit: the blur's dispatches must never enter the command
         * stream, so the frame must not contain them. */
        optics_gfx_color black = optics_gfx_color_rgba(0, 0, 0, 255);
        OT_EXPECT(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                      .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                      .frame = frame,
                                                      .clear_color = &black}) == OPTICS_GFX_OK);
        optics_gfx_canvas_draw_image(canvas, target, (optics_gfx_rect){0, 0, (float)W, (float)H},
                                     nullptr);
        OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);

        /* The plan's own accounting is the observable: the blur registered,
         * nothing executed it. */
        optics_gfx_frame_plan_counters stats = {};
        OT_EXPECT(optics_gfx_frame_plan_stats(frame, &stats) == OPTICS_GFX_OK);
        OT_EXPECT(stats.registered >= 1u);
        OT_EXPECT(stats.executed == 0u);
        OT_EXPECT(stats.pruned >= 1u);

        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);

        memset(px, 0, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
        /* The sharp capture reached the swapchain unchanged; the pruned blur
         * did not corrupt it. */
        OT_EXPECT(px[(H / 2 * W + 1) * 4 + 0] < 32);
        OT_EXPECT(px[(H / 2 * W + W - 2) * 4 + 0] > 224);
    }

    /* ---- 2. The same blur IS executed when materialized, and is correct ---- */
    {
        optics_gfx_frame *frame = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);

        optics_gfx_effect_blur_desc bd = OPTICS_GFX_EFFECT_BLUR_DESC_INIT;
        bd.input = target;
        bd.sigma = 6.0f;
        optics_gfx_image *blurred = nullptr;
        OT_EXPECT(optics_gfx_blur_filter_apply(
                      blur, &(optics_gfx_blur_filter_apply_opts){
                                .frame = frame, .desc = &bd, .out = &blurred}) == OPTICS_GFX_OK);
        OT_EXPECT(blurred != nullptr);

        /* Materialize before the pass that samples it: compute cannot be
         * inserted inside a render pass. */
        OT_EXPECT(optics_gfx_frame_materialize_image(frame, blurred) == OPTICS_GFX_OK);

        optics_gfx_frame_plan_counters stats = {};
        OT_EXPECT(optics_gfx_frame_plan_stats(frame, &stats) == OPTICS_GFX_OK);
        OT_EXPECT(stats.executed >= 1u);

        optics_gfx_color black = optics_gfx_color_rgba(0, 0, 0, 255);
        OT_EXPECT(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                      .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                      .frame = frame,
                                                      .clear_color = &black}) == OPTICS_GFX_OK);
        optics_gfx_canvas_draw_image(canvas, blurred, (optics_gfx_rect){0, 0, (float)W, (float)H},
                                     nullptr);
        OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);

        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);

        memset(px, 0, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
        /* A sigma=6 blur of a hard edge softens it: neither side of the
         * boundary is black or white any more. */
        uint8_t left = px[(H / 2 * W + (W / 2 - 1)) * 4 + 0];
        uint8_t right = px[(H / 2 * W + (W / 2)) * 4 + 0];
        OT_EXPECT(left > 16 && left < 240);
        OT_EXPECT(right > 16 && right < 240);
    }

    /* ---- 3. Sampling a deferred output inside a pass is rejected ---- */
    {
        optics_gfx_frame *frame = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);

        optics_gfx_effect_blur_desc bd = OPTICS_GFX_EFFECT_BLUR_DESC_INIT;
        bd.input = target;
        bd.sigma = 4.0f;
        optics_gfx_image *blurred = nullptr;
        OT_EXPECT(optics_gfx_blur_filter_apply(
                      blur, &(optics_gfx_blur_filter_apply_opts){
                                .frame = frame, .desc = &bd, .out = &blurred}) == OPTICS_GFX_OK);

        optics_gfx_color black = optics_gfx_color_rgba(0, 0, 0, 255);
        OT_EXPECT(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                      .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                      .frame = frame,
                                                      .clear_color = &black}) == OPTICS_GFX_OK);
        /* The draw is dropped and the error names the contract. */
        optics_gfx_canvas_draw_image(canvas, blurred, (optics_gfx_rect){0, 0, (float)W, (float)H},
                                     nullptr);
        optics_gfx_error_info err = {};
        optics_gfx_get_last_error(&err);
        OT_EXPECT(err.code == OPTICS_GFX_ERROR_INVALID_STATE);
        /* Materializing is refused for the same reason: a pass is active. */
        OT_EXPECT(optics_gfx_frame_materialize_image(frame, blurred) ==
                  OPTICS_GFX_ERROR_INVALID_STATE);
        OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
        /* After the boundary it is legal again. */
        OT_EXPECT(optics_gfx_frame_materialize_image(frame, blurred) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
    }

    /* ---- 4. A consumer makes its producer live through the chain ----
     *
     * Requiring only the consumer must also execute the producer it samples.
     * Two filters are needed because one filter owns exactly one output image
     * per frame slot, so chaining through a single filter would overwrite the
     * intermediate rather than build a chain. This is the link a chain
     * (blur -> blur) would otherwise lose, executing the consumer against an
     * unexecuted producer. */
    {
        optics_gfx_blur_filter *blur_b = nullptr;
        OT_EXPECT(optics_gfx_blur_filter_create(d, &blur_b) == OPTICS_GFX_OK);

        optics_gfx_frame *frame = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);

        optics_gfx_effect_blur_desc bd = OPTICS_GFX_EFFECT_BLUR_DESC_INIT;
        bd.input = target;
        bd.sigma = 3.0f;
        optics_gfx_image *stage1 = nullptr;
        OT_EXPECT(optics_gfx_blur_filter_apply(
                      blur, &(optics_gfx_blur_filter_apply_opts){
                                .frame = frame, .desc = &bd, .out = &stage1}) == OPTICS_GFX_OK);
        OT_EXPECT(stage1 != nullptr);

        /* A second pass sampling the first one's output. */
        optics_gfx_effect_blur_desc bd2 = OPTICS_GFX_EFFECT_BLUR_DESC_INIT;
        bd2.input = stage1;
        bd2.sigma = 3.0f;
        optics_gfx_image *stage2 = nullptr;
        OT_EXPECT(optics_gfx_blur_filter_apply(
                      blur_b, &(optics_gfx_blur_filter_apply_opts){
                                  .frame = frame, .desc = &bd2, .out = &stage2}) == OPTICS_GFX_OK);
        OT_EXPECT(stage2 != nullptr);
        OT_EXPECT(stage2 != stage1);

        optics_gfx_frame_plan_counters stats = {};
        OT_EXPECT(optics_gfx_frame_plan_stats(frame, &stats) == OPTICS_GFX_OK);
        OT_EXPECT(stats.registered == 2u);
        OT_EXPECT(stats.executed == 0u);

        /* Requiring only the consumer must pull the producer in. */
        OT_EXPECT(optics_gfx_frame_materialize_image(frame, stage2) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_plan_stats(frame, &stats) == OPTICS_GFX_OK);
        OT_EXPECT(stats.executed == 2u);
        OT_EXPECT(stats.pruned == 0u);

        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
        optics_gfx_blur_filter_release(blur_b);
    }

    /* ---- 5. A deferred pass labels its own body (ADR-0147/ADR-0133) ----
     *
     * The pass record carries a debug label. Before this assertion nothing read
     * it: it was stored at registration and never consulted, which is exactly
     * the "declared, never observed" shape ADR-0133 exists to prevent. Here the
     * label is emitted around the deferred body, so the counter that
     * test_pass_labels uses must move when a deferred pass executes — and must
     * not move when the pass is pruned. */
    {
        const uint64_t labels_before = optics_gfx_device_labels_emitted(d);

        optics_gfx_frame *frame = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);

        optics_gfx_effect_blur_desc bd = OPTICS_GFX_EFFECT_BLUR_DESC_INIT;
        bd.input = target;
        bd.sigma = 4.0f;
        optics_gfx_image *blurred = nullptr;
        OT_EXPECT(optics_gfx_blur_filter_apply(
                      blur, &(optics_gfx_blur_filter_apply_opts){
                                .frame = frame, .desc = &bd, .out = &blurred}) == OPTICS_GFX_OK);

        /* Nothing required it yet, so nothing has been built or labelled. */
        OT_EXPECT(optics_gfx_device_labels_emitted(d) == labels_before);

        OT_EXPECT(optics_gfx_frame_materialize_image(frame, blurred) == OPTICS_GFX_OK);

        /* Executing the deferred body emits its label. */
        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);

        /* Hard assertion, not a conditional one. This device requested the
         * capability, so a deferred pass that executes must emit its label. If
         * it does not, the label is dead storage again or the capability path
         * regressed. A conditional form would pass forever while observing
         * nothing.
         *
         * Exactly one: optics_gfx_vk_cmd_begin_label increments the counter and
         * end_label does not, matching the one-label-per-pass rule
         * test_pass_labels asserts for eager passes. Measured, not assumed —
         * asserting `> labels_before` alone would have accepted a pass emitting
         * any number of labels. */
        OT_EXPECT(optics_gfx_device_labels_emitted(d) == labels_before + 1u);
    }

    /* ---- 6. A material's dispatches are prunable too (ADR-0147) ----
     *
     * This is the assertion that Phase 2 exists for. Before it, every material
     * recorded eagerly: an apply that nothing sampled still cost its full
     * dispatch set (measured 4.8 ms/frame for 64 liquid-glass bodies on a
     * 1024x1024 target). Registering a pass must now be enough for the plan to
     * prune it when no consumer appears.
     */
    {
        optics_material_liquid_glass_filter *glass = nullptr;
        OT_EXPECT(optics_material_liquid_glass_filter_create(d, &glass) == OPTICS_GFX_OK);

        optics_gfx_image *slab_target = nullptr;
        OT_EXPECT(optics_gfx_image_create(
                      d,
                      &(optics_gfx_image_desc){.type = OPTICS_GFX_TYPE_IMAGE_DESC,
                                               .width = W,
                                               .height = H,
                                               .format = OPTICS_GFX_FORMAT_RGBA8_UNORM,
                                               .usage = OPTICS_GFX_IMAGE_USAGE_RENDER_TARGET},
                      &slab_target) == OPTICS_GFX_OK);

        optics_gfx_frame *frame = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);

        /* A blur the material samples, so the chain has both kinds of pass. */
        optics_gfx_effect_blur_desc bd = OPTICS_GFX_EFFECT_BLUR_DESC_INIT;
        bd.input = target;
        bd.sigma = 3.0f;
        optics_gfx_image *blurred = nullptr;
        OT_EXPECT(optics_gfx_blur_filter_apply(
                      blur, &(optics_gfx_blur_filter_apply_opts){
                                .frame = frame, .desc = &bd, .out = &blurred}) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_materialize_image(frame, blurred) == OPTICS_GFX_OK);

        optics_material_liquid_glass_shape shape = {
            .bounds = {4.0f, 4.0f, (float)W - 8.0f, (float)H - 8.0f},
            .corner_radius = 8.0f,
        };
        optics_material_liquid_glass_group group = OPTICS_MATERIAL_LIQUID_GLASS_GROUP_INIT;
        group.shapes = &shape;
        group.shape_count = 1u;

        optics_material_liquid_glass_desc gd = OPTICS_MATERIAL_LIQUID_GLASS_DESC_INIT;
        gd.input = target;
        gd.blurred_input = blurred;
        gd.groups = &group;
        gd.group_count = 1u;
        optics_material_output out = {};
        OT_EXPECT(optics_material_liquid_glass_filter_apply(
                      glass, &(optics_material_liquid_glass_filter_apply_opts){
                                 .frame = frame, .desc = &gd, .out = &out}) == OPTICS_GFX_OK);
        OT_EXPECT(out.image != nullptr);

        optics_gfx_frame_plan_counters stats = {};
        OT_EXPECT(optics_gfx_frame_plan_stats(frame, &stats) == OPTICS_GFX_OK);
        OT_EXPECT(stats.registered >= 2u);
        OT_EXPECT(stats.executed == 1u); /* the blur only: the material is still pending */

        /* Nothing draws `out.image`, so the material pass must be pruned while
         * the blur it needed still ran. */
        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_plan_stats(frame, &stats) == OPTICS_GFX_ERROR_INVALID_STATE);

        optics_gfx_image_release(slab_target);
        optics_material_liquid_glass_filter_release(glass);
    }

    /* ---- 7. The raw recording path cannot read a pending output ----
     *
     * `optics_gfx_effect_blur` records straight into the caller's command buffer: it
     * does not register a pass, cannot be pruned, and has no frame to consult, so
     * before this guard it happily sampled a deferred output whose producer had
     * never run — silently reading last frame's pixels under this frame's handle.
     * The demo that proves it is exactly this: feed it the deferred output.
     *
     * The fix must not be over-strict either: after materializing, the same call
     * must succeed. Both halves are asserted. */
    {
        optics_gfx_frame *frame = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);

        optics_gfx_effect_blur_desc bd = OPTICS_GFX_EFFECT_BLUR_DESC_INIT;
        bd.input = target;
        bd.sigma = 4.0f;
        optics_gfx_image *deferred = nullptr;
        OT_EXPECT(optics_gfx_blur_filter_apply(
                      blur, &(optics_gfx_blur_filter_apply_opts){
                                .frame = frame, .desc = &bd, .out = &deferred}) == OPTICS_GFX_OK);

        VkCommandBuffer cmd = optics_gfx_frame_vk_command_buffer(frame);
        optics_gfx_effect_blur_desc one_shot = OPTICS_GFX_EFFECT_BLUR_DESC_INIT;
        one_shot.input = deferred;
        one_shot.sigma = 4.0f;
        optics_gfx_image *out = nullptr;
        OT_EXPECT(optics_gfx_effect_blur(
                      &(optics_gfx_effect_blur_opts){.cmd = cmd, .desc = &one_shot, .out = &out}) ==
                  OPTICS_GFX_ERROR_INVALID_STATE);

        /* Materialize, then the same primitive must accept the input. */
        OT_EXPECT(optics_gfx_frame_materialize_image(frame, deferred) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_effect_blur(&(optics_gfx_effect_blur_opts){
                      .cmd = cmd, .desc = &one_shot, .out = &out}) == OPTICS_GFX_OK);
        OT_EXPECT(out != nullptr);

        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
    }

    /* ---- 8. Boundary capacity saturation: registering up to OPTICS_GFX_PLAN_MAX_PASSES (64) ----
     */
    {
        optics_gfx_frame *frame = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);

        optics_gfx_deferred_token *tokens[64] = {};
        for (uint32_t i = 0; i < 64u; ++i) {
            optics_gfx_deferred_pass_desc desc = OPTICS_GFX_DEFERRED_PASS_DESC_INIT;
            desc.record = dummy_pass_record;
            desc.output_count = 1u;
            desc.outputs[0] = target;
            desc.label = "bench.saturation";
            OT_EXPECT(optics_gfx_frame_register_pass(frame, &desc, &tokens[i]) == OPTICS_GFX_OK);
            OT_EXPECT(tokens[i] != nullptr);
        }

        optics_gfx_frame_plan_counters stats = {};
        OT_EXPECT(optics_gfx_frame_plan_stats(frame, &stats) == OPTICS_GFX_OK);
        OT_EXPECT(stats.registered == 64u);
        OT_EXPECT(stats.fallbacks == 0u);

        /* Require half of them: exactly 32 must execute, exactly 32 must prune */
        for (uint32_t i = 0; i < 32u; ++i) {
            OT_EXPECT(optics_gfx_frame_require_pass(frame, tokens[i]) == OPTICS_GFX_OK);
        }

        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
    }

    /* ---- 9. Capacity overflow & fallback: 65th pass degrades gracefully ---- */
    {
        optics_gfx_frame *frame = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);

        for (uint32_t i = 0; i < 64u; ++i) {
            optics_gfx_deferred_pass_desc desc = OPTICS_GFX_DEFERRED_PASS_DESC_INIT;
            desc.record = dummy_pass_record;
            desc.output_count = 1u;
            desc.outputs[0] = target;
            OT_EXPECT(optics_gfx_frame_register_pass(frame, &desc, nullptr) == OPTICS_GFX_OK);
        }

        /* The 65th pass must fail with OPTICS_GFX_ERROR_OUT_OF_RANGE */
        optics_gfx_deferred_pass_desc overflow = OPTICS_GFX_DEFERRED_PASS_DESC_INIT;
        overflow.record = dummy_pass_record;
        overflow.output_count = 1u;
        overflow.outputs[0] = target;
        OT_EXPECT(optics_gfx_frame_register_pass(frame, &overflow, nullptr) ==
                  OPTICS_GFX_ERROR_OUT_OF_RANGE);

        optics_gfx_frame_plan_counters stats = {};
        OT_EXPECT(optics_gfx_frame_plan_stats(frame, &stats) == OPTICS_GFX_OK);
        OT_EXPECT(stats.registered == 64u);
        OT_EXPECT(stats.fallbacks == 1u); /* fallback telemetry accurately incremented */

        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
    }

    /* ---- 10. Arena exhaustion & graceful degradation ---- */
    {
        optics_gfx_frame *frame = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);

        /* Try to register a pass requesting 50 KiB (exceeding 48 KiB ceiling) */
        char huge_params[64] = {};
        optics_gfx_deferred_pass_desc desc = OPTICS_GFX_DEFERRED_PASS_DESC_INIT;
        desc.record = dummy_pass_record;
        desc.output_count = 1u;
        desc.outputs[0] = target;
        desc.params = huge_params;
        desc.params_size = 50u * 1024u; /* 50 KiB > 48 KiB */
        OT_EXPECT(optics_gfx_frame_register_pass(frame, &desc, nullptr) ==
                  OPTICS_GFX_ERROR_OUT_OF_MEMORY);

        optics_gfx_frame_plan_counters stats = {};
        OT_EXPECT(optics_gfx_frame_plan_stats(frame, &stats) == OPTICS_GFX_OK);
        OT_EXPECT(stats.fallbacks == 1u);

        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
    }

    /* ---- 11. Foreign & stale token protection (Generational Epoch validation) ---- */
    {
        optics_gfx_frame *frame1 = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame1) == OPTICS_GFX_OK);

        optics_gfx_deferred_token *tok_f1 = nullptr;
        optics_gfx_deferred_pass_desc desc = OPTICS_GFX_DEFERRED_PASS_DESC_INIT;
        desc.record = dummy_pass_record;
        desc.output_count = 1u;
        desc.outputs[0] = target;
        OT_EXPECT(optics_gfx_frame_register_pass(frame1, &desc, &tok_f1) == OPTICS_GFX_OK);
        OT_EXPECT(tok_f1 != nullptr);

        OT_EXPECT(optics_gfx_frame_submit(frame1) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(frame1) == OPTICS_GFX_OK);

        /* Frame 2 begins: epoch increments */
        optics_gfx_frame *frame2 = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame2) == OPTICS_GFX_OK);

        /* Case 11A: Stale token from frame 1 must be rejected */
        OT_EXPECT(optics_gfx_frame_require_pass(frame2, tok_f1) ==
                  OPTICS_GFX_ERROR_INVALID_ARGUMENT);

        /* Case 11B: Random garbage pointer must be rejected */
        optics_gfx_deferred_token *bogus_token = (optics_gfx_deferred_token *)0xDEADBEEF;
        OT_EXPECT(optics_gfx_frame_require_pass(frame2, bogus_token) ==
                  OPTICS_GFX_ERROR_INVALID_ARGUMENT);

        OT_EXPECT(optics_gfx_frame_submit(frame2) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(frame2) == OPTICS_GFX_OK);
    }

    /* ---- 12. High-Frequency 1000-Frame Stress & Zero-Leak Endurance ---- */
    {
        for (uint32_t rep = 0; rep < 1000u; ++rep) {
            optics_gfx_frame *frame = nullptr;
            OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);

            optics_gfx_deferred_token *tok = nullptr;
            optics_gfx_deferred_pass_desc desc = OPTICS_GFX_DEFERRED_PASS_DESC_INIT;
            desc.record = dummy_pass_record;
            desc.output_count = 1u;
            desc.outputs[0] = target;
            OT_EXPECT(optics_gfx_frame_register_pass(frame, &desc, &tok) == OPTICS_GFX_OK);

            if (rep % 2 == 0) {
                OT_EXPECT(optics_gfx_frame_require_pass(frame, tok) == OPTICS_GFX_OK);
            }
            /* Otherwise left unrequired to be pruned */

            OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
            OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
        }
    }

    free(px);
    optics_gfx_effect_reset(d);
    optics_gfx_blur_filter_release(blur);
    optics_gfx_image_release(target);
    optics_gfx_canvas_release(canvas);
    optics_gfx_surface_release(s);
    optics_gfx_device_release(d);

    return OT_REPORT();
}
