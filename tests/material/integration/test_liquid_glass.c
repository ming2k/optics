/*
 * Liquid glass material (prism) over a canvas render-target capture
 * (ADR-0017): render canvas content into a flux_image, blur it with the
 * flux reusable blur filter, composite analytic glass bodies with the
 * prism material filter, and draw the result back onto a frame.
 *
 * Asserts:
 *   - the analytic rounded SDF masks every optical layer (bounding-box
 *     corner outside the body stays exactly sharp)
 *   - the persistent per-slot output clears exactly the previous + current
 *     footprints (moved/shrunk/disappeared bodies leave no residue, and
 *     disjoint regions are never joined into a bounding-box clear)
 *   - all clears are recorded before any material dispatch
 *   - a later body's translucent shadow source-overs an earlier body
 */
#include "gfx_test.h"
#include "optics_test.h"
#include <optics/gfx.h>
#include <optics/gfx/effect.h>
#include <optics/gfx/vulkan.h>
#include <optics/material.h>

#include "../material_test_helpers.h"

#include <stdlib.h>
#include <string.h>

#define W 64u
#define H 64u
#define BYTES (W * H * 4u)
#define TEST_FRAME_SLOTS 3u /* mirrors FLUX_MAX_FRAMES_IN_FLIGHT */

static uint32_t render_liquid_glass_frame(optics_gfx_surface *surface, optics_gfx_canvas *canvas,
                                          optics_gfx_image *target,
                                          optics_gfx_blur_filter *blur_filter,
                                          optics_material_liquid_glass_filter *glass_filter,
                                          const optics_material_liquid_glass_group *groups,
                                          uint32_t group_count, uint8_t *pixels) {
    optics_gfx_frame *frame = nullptr;
    OT_EXPECT(optics_gfx_surface_begin_frame(surface, nullptr, &frame) == OPTICS_GFX_OK);
    uint32_t slot = optics_gfx_frame_index(frame);

    optics_gfx_color source = optics_gfx_color_rgba(96, 144, 208, 255);
    OT_EXPECT(
        optics_gfx_canvas_begin(
            canvas, &(optics_gfx_canvas_pass_desc){
                        .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                        .frame = frame,
                        .attachment = {.kind = OPTICS_GFX_CANVAS_ATTACHMENT_IMAGE, .image = target},
                        .clear_color = &source}) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);

    optics_gfx_effect_blur_desc blur_desc = OPTICS_GFX_EFFECT_BLUR_DESC_INIT;
    blur_desc.input = target;
    blur_desc.sigma = 2.0f;
    optics_gfx_image *blurred = nullptr;
    OT_EXPECT(optics_gfx_blur_filter_apply(
                  blur_filter,
                  &(optics_gfx_blur_filter_apply_opts){
                      .frame = frame, .desc = &blur_desc, .out = &blurred}) == OPTICS_GFX_OK);

    optics_material_liquid_glass_desc glass_desc = OPTICS_MATERIAL_LIQUID_GLASS_DESC_INIT;
    glass_desc.input = target;
    glass_desc.blurred_input = blurred;
    glass_desc.groups = groups;
    glass_desc.group_count = group_count;
    glass_desc.refraction = 0.0f;
    optics_material_output glass = {};
    OT_EXPECT(optics_material_liquid_glass_filter_apply(
                  glass_filter,
                  &(optics_material_liquid_glass_filter_apply_opts){
                      .frame = frame, .desc = &glass_desc, .out = &glass}) == OPTICS_GFX_OK);

    optics_gfx_color black = optics_gfx_color_rgba(0, 0, 0, 255);
    OT_EXPECT(optics_gfx_canvas_begin(
                  canvas, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                         .frame = frame,
                                                         .clear_color = &black}) == OPTICS_GFX_OK);
    test_draw_material(canvas, glass);
    OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
    memset(pixels, 0xCD, BYTES);
    OT_EXPECT(optics_gfx_surface_read_pixels(surface, pixels, BYTES) == OPTICS_GFX_OK);
    return slot;
}

int main(void) {
    optics_gfx_device *d = optics_gfx_test_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_liquid_glass: no Vulkan device; skipping\n");
        OT_SKIP("no Vulkan device");
    }

    optics_gfx_surface_desc sd = OPTICS_GFX_SURFACE_DESC_INIT;
    sd.width = W;
    sd.height = H;
    optics_gfx_surface *s = nullptr;
    OT_EXPECT(optics_gfx_surface_create(d, &sd, &s) == OPTICS_GFX_OK);

    VkFormat sfmt = optics_gfx_surface_vk_format(s);
    optics_gfx_format target_fmt = (sfmt == VK_FORMAT_B8G8R8A8_UNORM)
                                       ? OPTICS_GFX_FORMAT_BGRA8_UNORM
                                       : OPTICS_GFX_FORMAT_RGBA8_UNORM;

    optics_gfx_canvas_desc cd = {.type = OPTICS_GFX_TYPE_CANVAS_DESC, .surface = s};
    optics_gfx_canvas *canvas = nullptr;
    OT_EXPECT(optics_gfx_canvas_create(&cd, &canvas) == OPTICS_GFX_OK);

    optics_gfx_image *target = nullptr;
    OT_EXPECT(optics_gfx_image_create(
                  d,
                  &(optics_gfx_image_desc){.type = OPTICS_GFX_TYPE_IMAGE_DESC,
                                           .width = W,
                                           .height = H,
                                           .format = target_fmt,
                                           .usage = OPTICS_GFX_IMAGE_USAGE_RENDER_TARGET},
                  &target) == OPTICS_GFX_OK);

    optics_gfx_blur_filter *blur_filter = nullptr;
    OT_EXPECT(optics_gfx_blur_filter_create(d, &blur_filter) == OPTICS_GFX_OK);
    optics_material_liquid_glass_filter *glass_filter = nullptr;
    OT_EXPECT(optics_material_liquid_glass_filter_create(d, &glass_filter) == OPTICS_GFX_OK);
    /* Pipeline pre-warming contract (ADR-0141) */
    OT_EXPECT(optics_material_liquid_glass_filter_warmup(nullptr, nullptr) ==
              OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(optics_material_liquid_glass_filter_warmup(glass_filter, nullptr) == OPTICS_GFX_OK);
    OT_EXPECT(optics_material_liquid_glass_filter_warmup(
                  glass_filter,
                  &(optics_material_liquid_glass_warmup_opts){
                      .format = OPTICS_GFX_FORMAT_RGBA8_UNORM, .stats = true}) == OPTICS_GFX_OK);

    static uint8_t px[BYTES];

    /* --- liquid glass: the analytic rounded SDF masks every optical layer --- */
    {
        optics_gfx_frame *frame = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);
        optics_gfx_color black = optics_gfx_color_rgba(0, 0, 0, 255);
        OT_EXPECT(optics_gfx_canvas_begin(
                      canvas, &(optics_gfx_canvas_pass_desc){
                                  .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                  .frame = frame,
                                  .attachment = {.kind = OPTICS_GFX_CANVAS_ATTACHMENT_IMAGE,
                                                 .image = target},
                                  .clear_color = &black}) == OPTICS_GFX_OK);
        optics_gfx_canvas_fill_rect_color(
            canvas, (optics_gfx_rect){(float)(W / 2), 0.0f, (float)(W / 2), (float)H},
            optics_gfx_color_rgba_premul(255, 255, 255, 255));
        OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);

        optics_gfx_effect_blur_desc bd = OPTICS_GFX_EFFECT_BLUR_DESC_INIT;
        bd.input = target;
        bd.sigma = 8.0f;
        optics_gfx_image *blurred = nullptr;
        OT_EXPECT(optics_gfx_blur_filter_apply(blur_filter,
                                               &(optics_gfx_blur_filter_apply_opts){
                                                   .frame = frame, .desc = &bd, .out = &blurred}) ==
                  OPTICS_GFX_OK);

        optics_material_liquid_glass_shape body_shapes[1] = {
            {.bounds = {16, 16, 32, 32}, .corner_radius = 16},
        };
        optics_material_liquid_glass_group body = OPTICS_MATERIAL_LIQUID_GLASS_GROUP_INIT;
        body.shapes = body_shapes;
        body.shape_count = 1;
        optics_material_liquid_glass_desc gd = OPTICS_MATERIAL_LIQUID_GLASS_DESC_INIT;
        gd.input = target;
        gd.blurred_input = blurred;
        gd.groups = &body;
        gd.group_count = 1;
        gd.refraction = 6.0f;
        optics_material_output glass = {};
        OT_EXPECT(
            optics_material_liquid_glass_filter_apply(
                glass_filter, &(optics_material_liquid_glass_filter_apply_opts){
                                  .frame = frame, .desc = &gd, .out = &glass}) == OPTICS_GFX_OK);
        OT_EXPECT(glass.image != nullptr);

        OT_EXPECT(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                      .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                      .frame = frame,
                                                      .clear_color = &black}) == OPTICS_GFX_OK);
        optics_gfx_canvas_draw_image(canvas, target, (optics_gfx_rect){0, 0, (float)W, (float)H},
                                     nullptr);
        test_draw_material(canvas, glass);
        OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
        memset(px, 0, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);

        /* The rounded-rect bounding-box corner is outside the SDF and stays
         * exactly sharp/black. The centre crosses the captured hard edge and
         * therefore contains visibly refracted/frosted intermediate values. */
        const uint8_t *dead_corner = &px[(16 * W + 16) * 4];
        const uint8_t *glass_centre = &px[(32 * W + 32) * 4];
        OT_EXPECT(dead_corner[0] < 5 && dead_corner[1] < 5 && dead_corner[2] < 5);
        OT_EXPECT(glass_centre[0] > 16 && glass_centre[0] < 245);
    }

    /* --- persistent liquid output clears only previous + current footprints --- */
    {
        optics_material_liquid_glass_filter *persistent_filter = nullptr;
        OT_EXPECT(optics_material_liquid_glass_filter_create(d, &persistent_filter) ==
                  OPTICS_GFX_OK);
        OT_EXPECT(optics_material_liquid_glass_filter_warmup(persistent_filter, nullptr) ==
                  OPTICS_GFX_OK);
        bool seen[TEST_FRAME_SLOTS] = {false};

        optics_material_liquid_glass_shape left_shapes[1] = {
            {.bounds = {4.0f, 24.0f, 16.0f, 16.0f}, .corner_radius = 8.0f},
        };
        optics_material_liquid_glass_group left = OPTICS_MATERIAL_LIQUID_GLASS_GROUP_INIT;
        left.shapes = left_shapes;
        left.shape_count = 1;
        for (uint32_t i = 0; i < TEST_FRAME_SLOTS; ++i) {
            uint32_t slot = render_liquid_glass_frame(s, canvas, target, blur_filter,
                                                      persistent_filter, &left, 1u, px);
            OT_EXPECT(slot < TEST_FRAME_SLOTS);
            if (slot < TEST_FRAME_SLOTS)
                seen[slot] = true;
            const uint8_t *left_centre = &px[(32u * W + 12u) * 4u];
            const uint8_t *right_centre = &px[(32u * W + 52u) * 4u];
            OT_EXPECT(left_centre[0] > 10u || left_centre[1] > 10u || left_centre[2] > 10u);
            OT_EXPECT(right_centre[0] < 5u && right_centre[1] < 5u && right_centre[2] < 5u);
        }
        /* Reusing each slot with a disjoint body must remove the old body
         * without clearing the empty middle of the output. */
        optics_material_liquid_glass_shape right_shapes[1] = {
            {.bounds = {44.0f, 24.0f, 16.0f, 16.0f}, .corner_radius = 8.0f},
        };
        optics_material_liquid_glass_group right = OPTICS_MATERIAL_LIQUID_GLASS_GROUP_INIT;
        right.shapes = right_shapes;
        right.shape_count = 1;
        bool reused_previous_slot = false;
        for (uint32_t i = 0; i < TEST_FRAME_SLOTS; ++i) {
            uint32_t slot = render_liquid_glass_frame(s, canvas, target, blur_filter,
                                                      persistent_filter, &right, 1u, px);
            OT_EXPECT(slot < TEST_FRAME_SLOTS);
            if (slot < TEST_FRAME_SLOTS && seen[slot])
                reused_previous_slot = true;
            const uint8_t *old_centre = &px[(32u * W + 12u) * 4u];
            const uint8_t *new_centre = &px[(32u * W + 52u) * 4u];
            const uint8_t *outside = &px[(4u * W + 32u) * 4u];
            OT_EXPECT(old_centre[0] < 5u && old_centre[1] < 5u && old_centre[2] < 5u);
            OT_EXPECT(new_centre[0] > 10u || new_centre[1] > 10u || new_centre[2] > 10u);
            OT_EXPECT(outside[0] < 5u && outside[1] < 5u && outside[2] < 5u);
        }
        OT_EXPECT(reused_previous_slot);

        /* All clear dispatches happen before material dispatches. The second
         * group's padded dispatch region covers x=28 but its SDF does not; an
         * incorrect clear-before-each-group implementation erases group 0 at
         * that pixel. */
        optics_material_liquid_glass_group overlapping[2] = {
            OPTICS_MATERIAL_LIQUID_GLASS_GROUP_INIT,
            OPTICS_MATERIAL_LIQUID_GLASS_GROUP_INIT,
        };
        optics_material_liquid_glass_shape overlapping_shapes[2][1] = {
            {{.bounds = {16.0f, 24.0f, 16.0f, 16.0f}, .corner_radius = 2.0f}},
            {{.bounds = {30.0f, 24.0f, 16.0f, 16.0f}, .corner_radius = 2.0f}},
        };
        overlapping[0].shapes = overlapping_shapes[0];
        overlapping[0].shape_count = 1;
        overlapping[1].shapes = overlapping_shapes[1];
        overlapping[1].shape_count = 1;
        bool overlap_seen[TEST_FRAME_SLOTS] = {false};
        for (uint32_t i = 0; i < TEST_FRAME_SLOTS; ++i) {
            uint32_t slot = render_liquid_glass_frame(s, canvas, target, blur_filter,
                                                      persistent_filter, overlapping, 2u, px);
            if (slot < TEST_FRAME_SLOTS)
                overlap_seen[slot] = true;
            const uint8_t *first_only = &px[(32u * W + 28u) * 4u];
            const uint8_t *second_centre = &px[(32u * W + 38u) * 4u];
            OT_EXPECT(first_only[0] > 10u || first_only[1] > 10u || first_only[2] > 10u);
            OT_EXPECT(second_centre[0] > 10u || second_centre[1] > 10u || second_centre[2] > 10u);
        }

        /* A later body's shadow is source-over material, not a replacement
         * for an earlier body. This mirrors a popover whose shadow footprint
         * reaches a Dock below it: the Dock must remain visible underneath
         * the translucent shadow instead of becoming a black cut-out. */
        optics_material_liquid_glass_group shadow_overlap[2] = {
            OPTICS_MATERIAL_LIQUID_GLASS_GROUP_INIT,
            OPTICS_MATERIAL_LIQUID_GLASS_GROUP_INIT,
        };
        optics_material_liquid_glass_shape shadow_shapes[2][1] = {
            {{.bounds = {8.0f, 36.0f, 48.0f, 20.0f}, .corner_radius = 6.0f}},
            {{.bounds = {16.0f, 8.0f, 32.0f, 20.0f}, .corner_radius = 6.0f}},
        };
        shadow_overlap[0].shapes = shadow_shapes[0];
        shadow_overlap[0].shape_count = 1;
        shadow_overlap[1].shapes = shadow_shapes[1];
        shadow_overlap[1].shape_count = 1;
        shadow_overlap[1].shadow_alpha = 0.7f;
        shadow_overlap[1].shadow_blur = 6.0f;
        shadow_overlap[1].shadow_offset_y = 6.0f;
        for (uint32_t i = 0; i < TEST_FRAME_SLOTS; ++i) {
            render_liquid_glass_frame(s, canvas, target, blur_filter, persistent_filter,
                                      shadow_overlap, 2u, px);
            const uint8_t *dock_under_shadow = &px[(40u * W + 32u) * 4u];
            OT_EXPECT(dock_under_shadow[0] > 10u || dock_under_shadow[1] > 10u ||
                      dock_under_shadow[2] > 10u);
        }

        /* An empty group list is the explicit disappearance operation: only
         * the previous footprints are cleared and the persistent image becomes
         * transparent again. */
        bool cleared_reused_slot = false;
        for (uint32_t i = 0; i < TEST_FRAME_SLOTS; ++i) {
            uint32_t slot = render_liquid_glass_frame(s, canvas, target, blur_filter,
                                                      persistent_filter, nullptr, 0u, px);
            if (slot < TEST_FRAME_SLOTS && overlap_seen[slot])
                cleared_reused_slot = true;
            const uint8_t *old_first = &px[(32u * W + 28u) * 4u];
            const uint8_t *old_second = &px[(32u * W + 38u) * 4u];
            OT_EXPECT(old_first[0] < 5u && old_first[1] < 5u && old_first[2] < 5u);
            OT_EXPECT(old_second[0] < 5u && old_second[1] < 5u && old_second[2] < 5u);
        }
        OT_EXPECT(cleared_reused_slot);
        optics_material_liquid_glass_filter_release(persistent_filter);
    }

    /* --- overlapping bodies composite immediately (ordering barrier gate) ---
     *
     * Groups source-over into one persistent image, and the renderer skips the
     * ordering barrier when a body's footprint is disjoint from every earlier
     * one (Vulkan never conflicts on disjoint stores, and image barriers cover
     * the whole subresource, so "no barrier" is only sound when the footprints
     * provably do not intersect). Overlapping bodies must therefore keep it.
     *
     * The discriminator is RGB, which tracks the body's opacity: a body's
     * premultiplied contribution is `colour * (coverage * opacity)`, so a 25%
     * body lands far darker than a 100% one (measured 45 vs 93 on the red
     * channel against this fixture). Render the shared pixel twice — once with a
     * lone 25%-opacity body, once with that body followed by a full-opacity body
     * at the identical footprint. Source-over of a full-opacity source over
     * anything is that source, so the stacked pixel must match the solid body,
     * not the faint one. If the second body's read of the destination raced the
     * first body's store, the composite is undefined.
     *
     * Note on what this gate can and cannot catch: it asserts the material
     * semantics (later body wins), which is what the mandatory barrier exists to
     * guarantee. Whether a *specific* GPU actually reorders in the absence of
     * the barrier is not observable from here — the deterministic half of this
     * decision (`liquid_glass_region_overlaps_any`) is unit-tested separately in
     * tests/prism/unit/test_liquid_glass_regions.c. */
    {
        optics_material_liquid_glass_group faint = OPTICS_MATERIAL_LIQUID_GLASS_GROUP_INIT;
        optics_material_liquid_glass_group solid = OPTICS_MATERIAL_LIQUID_GLASS_GROUP_INIT;
        /* Identical footprints: maximum overlap, so the barrier is mandatory. */
        optics_material_liquid_glass_shape faint_shape[1] = {
            {.bounds = {24.0f, 28.0f, 16.0f, 8.0f}, .corner_radius = 3.0f}};
        optics_material_liquid_glass_shape solid_shape[1] = {
            {.bounds = {24.0f, 28.0f, 16.0f, 8.0f}, .corner_radius = 3.0f}};
        faint.shapes = faint_shape;
        faint.shape_count = 1;
        faint.opacity = 0.25f;
        solid.shapes = solid_shape;
        solid.shape_count = 1;
        solid.opacity = 1.0f;

        optics_material_liquid_glass_group stack[2] = {faint, solid};

        uint32_t faint_red = 0u;
        uint32_t solid_red = 0u;
        uint32_t stacked_red = 0u;
        for (uint32_t i = 0; i < TEST_FRAME_SLOTS; ++i) {
            render_liquid_glass_frame(s, canvas, target, blur_filter, glass_filter, &faint, 1u, px);
            faint_red = px[(32u * W + 32u) * 4u];
            render_liquid_glass_frame(s, canvas, target, blur_filter, glass_filter, &solid, 1u, px);
            solid_red = px[(32u * W + 32u) * 4u];
            render_liquid_glass_frame(s, canvas, target, blur_filter, glass_filter, stack, 2u, px);
            stacked_red = px[(32u * W + 32u) * 4u];
        }
        /* The two references must be separated before the stacked value can
         * mean anything. */
        OT_EXPECT(solid_red > faint_red + 30u);
        /* Full-opacity over faint == full-opacity: the later body wins. */
        OT_EXPECT(stacked_red + 4u >= solid_red && stacked_red <= solid_red + 4u);
    }

    optics_gfx_device_wait_idle(d);
    optics_material_liquid_glass_filter_release(glass_filter);
    optics_gfx_blur_filter_release(blur_filter);
    optics_gfx_image_release(target);
    optics_gfx_canvas_release(canvas);
    optics_gfx_surface_release(s);
    optics_gfx_device_release(d);
    return OT_REPORT();
}
