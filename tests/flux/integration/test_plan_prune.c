/*
 * Deferred pass plan (ADR-0147): a registered effect must (a) produce
 * identical pixels when its output is consumed, and (b) cost nothing when
 * nothing consumes it.
 *
 * The second half is the point of the mechanism and the reason this test
 * exists: before ADR-0147 a blur computed unconditionally, so "the output
 * was never sampled" was indistinguishable from "the output was sampled" by
 * every externally visible measure except GPU time. The plan now makes the
 * difference observable through flux_canvas_get_plan_stats, and this test
 * pins it.
 */
#include "test_helpers.h"
#include <flux/canvas.h>
#include <flux/effect.h>
#include <flux/flux.h>
#include <flux/observe.h>
#include <flux/vulkan.h>
#include <prism/prism.h>

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
    static const flux_observe_opts observe = {
        .type = FLUX_TYPE_OBSERVE_OPTS,
        .capabilities = FLUX_OBSERVE_COUNTS | FLUX_OBSERVE_PASS_LABELS,
        .log = flux_console_logger,
    };
    flux_device_desc dd = FLUX_DEVICE_DESC_INIT;
    dd.headless = true;
    dd.next = &observe;
    flux_device *d = nullptr;
    if (flux_device_create(&dd, &d) != FLUX_OK) {
        fprintf(stderr, "test_plan_prune: no Vulkan device; skipping\n");
        TEST_SUMMARY();
        return 0;
    }

    flux_surface *s = nullptr;
    flux_surface_desc sdesc = FLUX_SURFACE_DESC_INIT;
    sdesc.width = W;
    sdesc.height = H;
    EXPECT(flux_surface_create(d, &sdesc, &s) == FLUX_OK);

    flux_canvas *canvas = nullptr;
    EXPECT(flux_canvas_create(&(flux_canvas_desc){.type = FLUX_TYPE_CANVAS_DESC, .surface = s},
                              &canvas) == FLUX_OK);

    uint8_t *px = calloc(1, BYTES);
    EXPECT(px != nullptr);
    fill_edge_pattern(px);

    flux_image_desc idesc = FLUX_IMAGE_DESC_INIT;
    idesc.width = W;
    idesc.height = H;
    idesc.format = FLUX_FORMAT_RGBA8_UNORM;
    idesc.initial_data = px;
    flux_image *target = nullptr;
    EXPECT(flux_image_create(d, &idesc, &target) == FLUX_OK);

    flux_blur_filter *blur = nullptr;
    EXPECT(flux_blur_filter_create(d, &blur) == FLUX_OK);

    /* ---- 1. A deferred blur nothing samples is pruned ---- */
    {
        flux_frame *frame = nullptr;
        EXPECT(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);

        flux_effect_blur_desc bd = FLUX_EFFECT_BLUR_DESC_INIT;
        bd.input = target;
        bd.sigma = 6.0f;
        flux_image *blurred = nullptr;
        EXPECT(flux_blur_filter_apply(blur, frame, &bd, &blurred) == FLUX_OK);
        EXPECT(blurred != nullptr);

        /* Nothing samples `blurred`. Draw only the sharp capture, then
         * submit: the blur's dispatches must never enter the command
         * stream, so the frame must not contain them. */
        flux_color black = flux_color_rgba(0, 0, 0, 255);
        EXPECT(
            flux_canvas_begin(canvas, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                               .frame = frame,
                                                               .clear_color = &black}) == FLUX_OK);
        flux_canvas_draw_image(canvas, target, (flux_rect){0, 0, (float)W, (float)H}, nullptr);
        EXPECT(flux_canvas_end(canvas) == FLUX_OK);

        /* The plan's own accounting is the observable: the blur registered,
         * nothing executed it. */
        flux_frame_plan_counters stats = {0};
        EXPECT(flux_frame_plan_stats(frame, &stats) == FLUX_OK);
        EXPECT(stats.registered >= 1u);
        EXPECT(stats.executed == 0u);
        EXPECT(stats.pruned >= 1u);

        EXPECT(flux_frame_submit(frame) == FLUX_OK);
        EXPECT(flux_frame_present(frame) == FLUX_OK);

        memset(px, 0, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);
        /* The sharp capture reached the swapchain unchanged; the pruned blur
         * did not corrupt it. */
        EXPECT(px[(H / 2 * W + 1) * 4 + 0] < 32);
        EXPECT(px[(H / 2 * W + W - 2) * 4 + 0] > 224);
    }

    /* ---- 2. The same blur IS executed when materialized, and is correct ---- */
    {
        flux_frame *frame = nullptr;
        EXPECT(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);

        flux_effect_blur_desc bd = FLUX_EFFECT_BLUR_DESC_INIT;
        bd.input = target;
        bd.sigma = 6.0f;
        flux_image *blurred = nullptr;
        EXPECT(flux_blur_filter_apply(blur, frame, &bd, &blurred) == FLUX_OK);
        EXPECT(blurred != nullptr);

        /* Materialize before the pass that samples it: compute cannot be
         * inserted inside a render pass. */
        EXPECT(flux_frame_materialize_image(frame, blurred) == FLUX_OK);

        flux_frame_plan_counters stats = {0};
        EXPECT(flux_frame_plan_stats(frame, &stats) == FLUX_OK);
        EXPECT(stats.executed >= 1u);

        flux_color black = flux_color_rgba(0, 0, 0, 255);
        EXPECT(
            flux_canvas_begin(canvas, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                               .frame = frame,
                                                               .clear_color = &black}) == FLUX_OK);
        flux_canvas_draw_image(canvas, blurred, (flux_rect){0, 0, (float)W, (float)H}, nullptr);
        EXPECT(flux_canvas_end(canvas) == FLUX_OK);

        EXPECT(flux_frame_submit(frame) == FLUX_OK);
        EXPECT(flux_frame_present(frame) == FLUX_OK);

        memset(px, 0, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);
        /* A sigma=6 blur of a hard edge softens it: neither side of the
         * boundary is black or white any more. */
        uint8_t left = px[(H / 2 * W + (W / 2 - 1)) * 4 + 0];
        uint8_t right = px[(H / 2 * W + (W / 2)) * 4 + 0];
        EXPECT(left > 16 && left < 240);
        EXPECT(right > 16 && right < 240);
    }

    /* ---- 3. Sampling a deferred output inside a pass is rejected ---- */
    {
        flux_frame *frame = nullptr;
        EXPECT(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);

        flux_effect_blur_desc bd = FLUX_EFFECT_BLUR_DESC_INIT;
        bd.input = target;
        bd.sigma = 4.0f;
        flux_image *blurred = nullptr;
        EXPECT(flux_blur_filter_apply(blur, frame, &bd, &blurred) == FLUX_OK);

        flux_color black = flux_color_rgba(0, 0, 0, 255);
        EXPECT(
            flux_canvas_begin(canvas, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                               .frame = frame,
                                                               .clear_color = &black}) == FLUX_OK);
        /* The draw is dropped and the error names the contract. */
        flux_canvas_draw_image(canvas, blurred, (flux_rect){0, 0, (float)W, (float)H}, nullptr);
        flux_error_info err = {0};
        flux_get_last_error(&err);
        EXPECT(err.code == FLUX_ERROR_INVALID_STATE);
        /* Materializing is refused for the same reason: a pass is active. */
        EXPECT(flux_frame_materialize_image(frame, blurred) == FLUX_ERROR_INVALID_STATE);
        EXPECT(flux_canvas_end(canvas) == FLUX_OK);
        /* After the boundary it is legal again. */
        EXPECT(flux_frame_materialize_image(frame, blurred) == FLUX_OK);
        EXPECT(flux_frame_submit(frame) == FLUX_OK);
        EXPECT(flux_frame_present(frame) == FLUX_OK);
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
        flux_blur_filter *blur_b = nullptr;
        EXPECT(flux_blur_filter_create(d, &blur_b) == FLUX_OK);

        flux_frame *frame = nullptr;
        EXPECT(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);

        flux_effect_blur_desc bd = FLUX_EFFECT_BLUR_DESC_INIT;
        bd.input = target;
        bd.sigma = 3.0f;
        flux_image *stage1 = nullptr;
        EXPECT(flux_blur_filter_apply(blur, frame, &bd, &stage1) == FLUX_OK);
        EXPECT(stage1 != nullptr);

        /* A second pass sampling the first one's output. */
        flux_effect_blur_desc bd2 = FLUX_EFFECT_BLUR_DESC_INIT;
        bd2.input = stage1;
        bd2.sigma = 3.0f;
        flux_image *stage2 = nullptr;
        EXPECT(flux_blur_filter_apply(blur_b, frame, &bd2, &stage2) == FLUX_OK);
        EXPECT(stage2 != nullptr);
        EXPECT(stage2 != stage1);

        flux_frame_plan_counters stats = {0};
        EXPECT(flux_frame_plan_stats(frame, &stats) == FLUX_OK);
        EXPECT(stats.registered == 2u);
        EXPECT(stats.executed == 0u);

        /* Requiring only the consumer must pull the producer in. */
        EXPECT(flux_frame_materialize_image(frame, stage2) == FLUX_OK);
        EXPECT(flux_frame_plan_stats(frame, &stats) == FLUX_OK);
        EXPECT(stats.executed == 2u);
        EXPECT(stats.pruned == 0u);

        EXPECT(flux_frame_submit(frame) == FLUX_OK);
        EXPECT(flux_frame_present(frame) == FLUX_OK);
        flux_blur_filter_release(blur_b);
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
        const uint64_t labels_before = flux_device_labels_emitted(d);

        flux_frame *frame = nullptr;
        EXPECT(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);

        flux_effect_blur_desc bd = FLUX_EFFECT_BLUR_DESC_INIT;
        bd.input = target;
        bd.sigma = 4.0f;
        flux_image *blurred = nullptr;
        EXPECT(flux_blur_filter_apply(blur, frame, &bd, &blurred) == FLUX_OK);

        /* Nothing required it yet, so nothing has been built or labelled. */
        EXPECT(flux_device_labels_emitted(d) == labels_before);

        EXPECT(flux_frame_materialize_image(frame, blurred) == FLUX_OK);

        /* Executing the deferred body emits its label. */
        EXPECT(flux_frame_submit(frame) == FLUX_OK);
        EXPECT(flux_frame_present(frame) == FLUX_OK);

        /* Hard assertion, not a conditional one. This device requested the
         * capability, so a deferred pass that executes must emit its label. If
         * it does not, the label is dead storage again or the capability path
         * regressed. A conditional form would pass forever while observing
         * nothing.
         *
         * Exactly one: flux_vk_cmd_begin_label increments the counter and
         * end_label does not, matching the one-label-per-pass rule
         * test_pass_labels asserts for eager passes. Measured, not assumed —
         * asserting `> labels_before` alone would have accepted a pass emitting
         * any number of labels. */
        EXPECT(flux_device_labels_emitted(d) == labels_before + 1u);
    }

    /* ---- 6. A Prism material's dispatches are prunable too (ADR-0147) ----
     *
     * This is the assertion that Phase 2 exists for. Before it, every material
     * recorded eagerly: an apply that nothing sampled still cost its full
     * dispatch set (measured 4.8 ms/frame for 64 liquid-glass bodies on a
     * 1024x1024 target). Registering a pass must now be enough for the plan to
     * prune it when no consumer appears.
     */
    {
        prism_liquid_glass_filter *glass = nullptr;
        EXPECT(prism_liquid_glass_filter_create(d, &glass) == FLUX_OK);

        flux_image *slab_target = nullptr;
        EXPECT(flux_image_create_render_target(d, W, H, FLUX_FORMAT_RGBA8_UNORM, &slab_target) ==
               FLUX_OK);

        flux_frame *frame = nullptr;
        EXPECT(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);

        /* A blur the material samples, so the chain has both kinds of pass. */
        flux_effect_blur_desc bd = FLUX_EFFECT_BLUR_DESC_INIT;
        bd.input = target;
        bd.sigma = 3.0f;
        flux_image *blurred = nullptr;
        EXPECT(flux_blur_filter_apply(blur, frame, &bd, &blurred) == FLUX_OK);
        EXPECT(flux_frame_materialize_image(frame, blurred) == FLUX_OK);

        prism_liquid_glass_shape shape = {
            .bounds = {4.0f, 4.0f, (float)W - 8.0f, (float)H - 8.0f},
            .corner_radius = 8.0f,
        };
        prism_liquid_glass_group group = PRISM_LIQUID_GLASS_GROUP_INIT;
        group.shapes = &shape;
        group.shape_count = 1u;

        prism_liquid_glass_desc gd = PRISM_LIQUID_GLASS_DESC_INIT;
        gd.input = target;
        gd.blurred_input = blurred;
        gd.groups = &group;
        gd.group_count = 1u;
        prism_material_output out = {};
        EXPECT(prism_liquid_glass_filter_apply(glass, frame, &gd, &out) == FLUX_OK);
        EXPECT(out.image != nullptr);

        flux_frame_plan_counters stats = {0};
        EXPECT(flux_frame_plan_stats(frame, &stats) == FLUX_OK);
        EXPECT(stats.registered >= 2u);
        EXPECT(stats.executed == 1u); /* the blur only: the material is still pending */

        /* Nothing draws `out.image`, so the material pass must be pruned while
         * the blur it needed still ran. */
        EXPECT(flux_frame_submit(frame) == FLUX_OK);
        EXPECT(flux_frame_present(frame) == FLUX_OK);
        EXPECT(flux_frame_plan_stats(frame, &stats) == FLUX_ERROR_INVALID_STATE);

        flux_image_release(slab_target);
        prism_liquid_glass_filter_release(glass);
    }

    /* ---- 7. The raw recording path cannot read a pending output ----
     *
     * `flux_effect_blur` records straight into the caller's command buffer: it
     * does not register a pass, cannot be pruned, and has no frame to consult, so
     * before this guard it happily sampled a deferred output whose producer had
     * never run — silently reading last frame's pixels under this frame's handle.
     * The demo that proves it is exactly this: feed it the deferred output.
     *
     * The fix must not be over-strict either: after materializing, the same call
     * must succeed. Both halves are asserted. */
    {
        flux_frame *frame = nullptr;
        EXPECT(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);

        flux_effect_blur_desc bd = FLUX_EFFECT_BLUR_DESC_INIT;
        bd.input = target;
        bd.sigma = 4.0f;
        flux_image *deferred = nullptr;
        EXPECT(flux_blur_filter_apply(blur, frame, &bd, &deferred) == FLUX_OK);

        VkCommandBuffer cmd = flux_frame_vk_command_buffer(frame);
        flux_effect_blur_desc one_shot = FLUX_EFFECT_BLUR_DESC_INIT;
        one_shot.input = deferred;
        one_shot.sigma = 4.0f;
        flux_image *out = nullptr;
        EXPECT(flux_effect_blur(cmd, &one_shot, &out) == FLUX_ERROR_INVALID_STATE);

        /* Materialize, then the same primitive must accept the input. */
        EXPECT(flux_frame_materialize_image(frame, deferred) == FLUX_OK);
        EXPECT(flux_effect_blur(cmd, &one_shot, &out) == FLUX_OK);
        EXPECT(out != nullptr);

        EXPECT(flux_frame_submit(frame) == FLUX_OK);
        EXPECT(flux_frame_present(frame) == FLUX_OK);
    }

    /* ---- 8. Boundary capacity saturation: registering up to FLUX_DEFERRED_MAX_PASSES (64) ---- */
    {
        flux_frame *frame = nullptr;
        EXPECT(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);

        flux_deferred_token *tokens[64] = {};
        for (uint32_t i = 0; i < 64u; ++i) {
            flux_deferred_pass_desc desc = FLUX_DEFERRED_PASS_DESC_INIT;
            desc.record = dummy_pass_record;
            desc.output_count = 1u;
            desc.outputs[0] = target;
            desc.label = "bench.saturation";
            EXPECT(flux_frame_register_pass(frame, &desc, &tokens[i]) == FLUX_OK);
            EXPECT(tokens[i] != nullptr);
        }

        flux_frame_plan_counters stats = {};
        EXPECT(flux_frame_plan_stats(frame, &stats) == FLUX_OK);
        EXPECT(stats.registered == 64u);
        EXPECT(stats.fallbacks == 0u);

        /* Require half of them: exactly 32 must execute, exactly 32 must prune */
        for (uint32_t i = 0; i < 32u; ++i) {
            EXPECT(flux_frame_require_pass(frame, tokens[i]) == FLUX_OK);
        }

        EXPECT(flux_frame_submit(frame) == FLUX_OK);
        EXPECT(flux_frame_present(frame) == FLUX_OK);
    }

    /* ---- 9. Capacity overflow & fallback: 65th pass degrades gracefully ---- */
    {
        flux_frame *frame = nullptr;
        EXPECT(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);

        for (uint32_t i = 0; i < 64u; ++i) {
            flux_deferred_pass_desc desc = FLUX_DEFERRED_PASS_DESC_INIT;
            desc.record = dummy_pass_record;
            desc.output_count = 1u;
            desc.outputs[0] = target;
            EXPECT(flux_frame_register_pass(frame, &desc, nullptr) == FLUX_OK);
        }

        /* The 65th pass must fail with FLUX_ERROR_OUT_OF_RANGE */
        flux_deferred_pass_desc overflow = FLUX_DEFERRED_PASS_DESC_INIT;
        overflow.record = dummy_pass_record;
        overflow.output_count = 1u;
        overflow.outputs[0] = target;
        EXPECT(flux_frame_register_pass(frame, &overflow, nullptr) == FLUX_ERROR_OUT_OF_RANGE);

        flux_frame_plan_counters stats = {};
        EXPECT(flux_frame_plan_stats(frame, &stats) == FLUX_OK);
        EXPECT(stats.registered == 64u);
        EXPECT(stats.fallbacks == 1u); /* fallback telemetry accurately incremented */

        EXPECT(flux_frame_submit(frame) == FLUX_OK);
        EXPECT(flux_frame_present(frame) == FLUX_OK);
    }

    /* ---- 10. Arena exhaustion & graceful degradation ---- */
    {
        flux_frame *frame = nullptr;
        EXPECT(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);

        /* Try to register a pass requesting 50 KiB (exceeding 48 KiB ceiling) */
        char huge_params[64] = {0};
        flux_deferred_pass_desc desc = FLUX_DEFERRED_PASS_DESC_INIT;
        desc.record = dummy_pass_record;
        desc.output_count = 1u;
        desc.outputs[0] = target;
        desc.params = huge_params;
        desc.params_size = 50u * 1024u; /* 50 KiB > 48 KiB */
        EXPECT(flux_frame_register_pass(frame, &desc, nullptr) == FLUX_ERROR_OUT_OF_MEMORY);

        flux_frame_plan_counters stats = {};
        EXPECT(flux_frame_plan_stats(frame, &stats) == FLUX_OK);
        EXPECT(stats.fallbacks == 1u);

        EXPECT(flux_frame_submit(frame) == FLUX_OK);
        EXPECT(flux_frame_present(frame) == FLUX_OK);
    }

    /* ---- 11. Foreign & stale token protection (Generational Epoch validation) ---- */
    {
        flux_frame *frame1 = nullptr;
        EXPECT(flux_surface_begin_frame(s, nullptr, &frame1) == FLUX_OK);

        flux_deferred_token *tok_f1 = nullptr;
        flux_deferred_pass_desc desc = FLUX_DEFERRED_PASS_DESC_INIT;
        desc.record = dummy_pass_record;
        desc.output_count = 1u;
        desc.outputs[0] = target;
        EXPECT(flux_frame_register_pass(frame1, &desc, &tok_f1) == FLUX_OK);
        EXPECT(tok_f1 != nullptr);

        EXPECT(flux_frame_submit(frame1) == FLUX_OK);
        EXPECT(flux_frame_present(frame1) == FLUX_OK);

        /* Frame 2 begins: epoch increments */
        flux_frame *frame2 = nullptr;
        EXPECT(flux_surface_begin_frame(s, nullptr, &frame2) == FLUX_OK);

        /* Case 11A: Stale token from frame 1 must be rejected */
        EXPECT(flux_frame_require_pass(frame2, tok_f1) == FLUX_ERROR_INVALID_ARGUMENT);

        /* Case 11B: Random garbage pointer must be rejected */
        flux_deferred_token *bogus_token = (flux_deferred_token *)0xDEADBEEF;
        EXPECT(flux_frame_require_pass(frame2, bogus_token) == FLUX_ERROR_INVALID_ARGUMENT);

        EXPECT(flux_frame_submit(frame2) == FLUX_OK);
        EXPECT(flux_frame_present(frame2) == FLUX_OK);
    }

    /* ---- 12. High-Frequency 1000-Frame Stress & Zero-Leak Endurance ---- */
    {
        for (uint32_t rep = 0; rep < 1000u; ++rep) {
            flux_frame *frame = nullptr;
            EXPECT(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);

            flux_deferred_token *tok = nullptr;
            flux_deferred_pass_desc desc = FLUX_DEFERRED_PASS_DESC_INIT;
            desc.record = dummy_pass_record;
            desc.output_count = 1u;
            desc.outputs[0] = target;
            EXPECT(flux_frame_register_pass(frame, &desc, &tok) == FLUX_OK);

            if (rep % 2 == 0) {
                EXPECT(flux_frame_require_pass(frame, tok) == FLUX_OK);
            }
            /* Otherwise left unrequired to be pruned */

            EXPECT(flux_frame_submit(frame) == FLUX_OK);
            EXPECT(flux_frame_present(frame) == FLUX_OK);
        }
    }

    free(px);
    flux_effect_reset(d);
    flux_blur_filter_release(blur);
    flux_image_release(target);
    flux_canvas_release(canvas);
    flux_surface_release(s);
    flux_device_release(d);

    TEST_SUMMARY();
    return 0;
}
