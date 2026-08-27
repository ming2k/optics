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
#include "test_helpers.h"
#include <flux/effect.h>
#include <flux/flux.h>
#include <flux/vulkan.h>
#include <prism/prism.h>

#include <stdlib.h>
#include <string.h>

#define W 64u
#define H 64u
#define BYTES (W * H * 4u)
#define TEST_FRAME_SLOTS 3u /* mirrors FLUX_MAX_FRAMES_IN_FLIGHT */

static uint32_t render_liquid_glass_frame(flux_surface *surface, flux_canvas *canvas,
                                          flux_image *target, flux_blur_filter *blur_filter,
                                          prism_liquid_glass_filter *glass_filter,
                                          const prism_liquid_glass_group *groups,
                                          uint32_t group_count, uint8_t *pixels) {
    flux_frame *frame = nullptr;
    EXPECT(flux_surface_begin_frame(surface, nullptr, &frame) == FLUX_OK);
    uint32_t slot = flux_frame_index(frame);

    flux_color source = flux_color_rgba(96, 144, 208, 255);
    EXPECT(flux_canvas_begin_target(canvas, frame, target, &source) == FLUX_OK);
    flux_canvas_end_target(canvas);

    flux_effect_blur_desc blur_desc = FLUX_EFFECT_BLUR_DESC_INIT;
    blur_desc.input = target;
    blur_desc.sigma = 2.0f;
    flux_image *blurred = nullptr;
    EXPECT(flux_blur_filter_apply(blur_filter, frame, &blur_desc, &blurred) == FLUX_OK);

    prism_liquid_glass_desc glass_desc = PRISM_LIQUID_GLASS_DESC_INIT;
    glass_desc.input = target;
    glass_desc.blurred_input = blurred;
    glass_desc.groups = groups;
    glass_desc.group_count = group_count;
    glass_desc.refraction = 0.0f;
    flux_image *glass = nullptr;
    EXPECT(prism_liquid_glass_filter_apply(glass_filter, frame, &glass_desc, &glass) == FLUX_OK);

    flux_color black = flux_color_rgba(0, 0, 0, 255);
    EXPECT(flux_canvas_begin_frame(canvas, frame, &black) == FLUX_OK);
    flux_canvas_draw_image(canvas, glass, (flux_rect){0, 0, (float)W, (float)H}, nullptr);
    flux_canvas_end_frame(canvas);
    EXPECT(flux_frame_submit(frame) == FLUX_OK);
    EXPECT(flux_frame_present(frame) == FLUX_OK);
    memset(pixels, 0xCD, BYTES);
    EXPECT(flux_surface_read_pixels(surface, pixels, BYTES) == FLUX_OK);
    return slot;
}

int main(void) {
    flux_device *d = test_helpers_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_liquid_glass: no Vulkan device; skipping\n");
        TEST_SUMMARY();
        return 0;
    }

    flux_surface_desc sd = FLUX_SURFACE_DESC_INIT;
    sd.width = W;
    sd.height = H;
    flux_surface *s = nullptr;
    EXPECT(flux_surface_create(d, &sd, &s) == FLUX_OK);

    VkFormat sfmt = flux_surface_vk_format(s);
    flux_format target_fmt =
        (sfmt == VK_FORMAT_B8G8R8A8_UNORM) ? FLUX_FORMAT_BGRA8_UNORM : FLUX_FORMAT_RGBA8_UNORM;

    flux_canvas_desc cd = {.type = FLUX_TYPE_CANVAS_DESC, .surface = s};
    flux_canvas *canvas = nullptr;
    EXPECT(flux_canvas_create(&cd, &canvas) == FLUX_OK);

    flux_image *target = nullptr;
    EXPECT(flux_image_create_render_target(d, W, H, target_fmt, &target) == FLUX_OK);

    flux_blur_filter *blur_filter = nullptr;
    EXPECT(flux_blur_filter_create(d, &blur_filter) == FLUX_OK);
    prism_liquid_glass_filter *glass_filter = nullptr;
    EXPECT(prism_liquid_glass_filter_create(d, &glass_filter) == FLUX_OK);

    static uint8_t px[BYTES];

    /* --- liquid glass: the analytic rounded SDF masks every optical layer --- */
    {
        flux_frame *frame = nullptr;
        EXPECT(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);
        flux_color black = flux_color_rgba(0, 0, 0, 255);
        EXPECT(flux_canvas_begin_target(canvas, frame, target, &black) == FLUX_OK);
        flux_canvas_fill_rect_color(canvas,
                                    (flux_rect){(float)(W / 2), 0.0f, (float)(W / 2), (float)H},
                                    flux_color_rgba_premul(255, 255, 255, 255));
        flux_canvas_end_target(canvas);

        flux_effect_blur_desc bd = FLUX_EFFECT_BLUR_DESC_INIT;
        bd.input = target;
        bd.sigma = 8.0f;
        flux_image *blurred = nullptr;
        EXPECT(flux_blur_filter_apply(blur_filter, frame, &bd, &blurred) == FLUX_OK);

        prism_liquid_glass_group body = PRISM_LIQUID_GLASS_GROUP_INIT;
        body.shapes[0] =
            (prism_liquid_glass_shape){.bounds = {16, 16, 32, 32}, .corner_radius = 16};
        prism_liquid_glass_desc gd = PRISM_LIQUID_GLASS_DESC_INIT;
        gd.input = target;
        gd.blurred_input = blurred;
        gd.groups = &body;
        gd.group_count = 1;
        gd.refraction = 6.0f;
        flux_image *glass = nullptr;
        EXPECT(prism_liquid_glass_filter_apply(glass_filter, frame, &gd, &glass) == FLUX_OK);
        EXPECT(glass != nullptr);

        EXPECT(flux_canvas_begin_frame(canvas, frame, &black) == FLUX_OK);
        flux_canvas_draw_image(canvas, target, (flux_rect){0, 0, (float)W, (float)H}, nullptr);
        flux_canvas_draw_image(canvas, glass, (flux_rect){0, 0, (float)W, (float)H}, nullptr);
        flux_canvas_end_frame(canvas);
        EXPECT(flux_frame_submit(frame) == FLUX_OK);
        EXPECT(flux_frame_present(frame) == FLUX_OK);
        memset(px, 0, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);

        /* The rounded-rect bounding-box corner is outside the SDF and stays
         * exactly sharp/black. The centre crosses the captured hard edge and
         * therefore contains visibly refracted/frosted intermediate values. */
        const uint8_t *dead_corner = &px[(16 * W + 16) * 4];
        const uint8_t *glass_centre = &px[(32 * W + 32) * 4];
        EXPECT(dead_corner[0] < 5 && dead_corner[1] < 5 && dead_corner[2] < 5);
        EXPECT(glass_centre[0] > 16 && glass_centre[0] < 245);
    }

    /* --- persistent liquid output clears only previous + current footprints --- */
    {
        prism_liquid_glass_filter *persistent_filter = nullptr;
        EXPECT(prism_liquid_glass_filter_create(d, &persistent_filter) == FLUX_OK);
        bool seen[TEST_FRAME_SLOTS] = {false};

        prism_liquid_glass_group left = PRISM_LIQUID_GLASS_GROUP_INIT;
        left.shapes[0] = (prism_liquid_glass_shape){
            .bounds = {4.0f, 24.0f, 16.0f, 16.0f},
            .corner_radius = 8.0f,
        };
        for (uint32_t i = 0; i < TEST_FRAME_SLOTS; ++i) {
            uint32_t slot = render_liquid_glass_frame(s, canvas, target, blur_filter,
                                                      persistent_filter, &left, 1u, px);
            EXPECT(slot < TEST_FRAME_SLOTS);
            if (slot < TEST_FRAME_SLOTS)
                seen[slot] = true;
            const uint8_t *left_centre = &px[(32u * W + 12u) * 4u];
            const uint8_t *right_centre = &px[(32u * W + 52u) * 4u];
            EXPECT(left_centre[0] > 10u || left_centre[1] > 10u || left_centre[2] > 10u);
            EXPECT(right_centre[0] < 5u && right_centre[1] < 5u && right_centre[2] < 5u);
        }
        /* Reusing each slot with a disjoint body must remove the old body
         * without clearing the empty middle of the output. */
        prism_liquid_glass_group right = PRISM_LIQUID_GLASS_GROUP_INIT;
        right.shapes[0] = (prism_liquid_glass_shape){
            .bounds = {44.0f, 24.0f, 16.0f, 16.0f},
            .corner_radius = 8.0f,
        };
        bool reused_previous_slot = false;
        for (uint32_t i = 0; i < TEST_FRAME_SLOTS; ++i) {
            uint32_t slot = render_liquid_glass_frame(s, canvas, target, blur_filter,
                                                      persistent_filter, &right, 1u, px);
            EXPECT(slot < TEST_FRAME_SLOTS);
            if (slot < TEST_FRAME_SLOTS && seen[slot])
                reused_previous_slot = true;
            const uint8_t *old_centre = &px[(32u * W + 12u) * 4u];
            const uint8_t *new_centre = &px[(32u * W + 52u) * 4u];
            const uint8_t *outside = &px[(4u * W + 32u) * 4u];
            EXPECT(old_centre[0] < 5u && old_centre[1] < 5u && old_centre[2] < 5u);
            EXPECT(new_centre[0] > 10u || new_centre[1] > 10u || new_centre[2] > 10u);
            EXPECT(outside[0] < 5u && outside[1] < 5u && outside[2] < 5u);
        }
        EXPECT(reused_previous_slot);

        /* All clear dispatches happen before material dispatches. The second
         * group's padded dispatch region covers x=28 but its SDF does not; an
         * incorrect clear-before-each-group implementation erases group 0 at
         * that pixel. */
        prism_liquid_glass_group overlapping[2] = {
            PRISM_LIQUID_GLASS_GROUP_INIT,
            PRISM_LIQUID_GLASS_GROUP_INIT,
        };
        overlapping[0].shapes[0] = (prism_liquid_glass_shape){
            .bounds = {16.0f, 24.0f, 16.0f, 16.0f},
            .corner_radius = 2.0f,
        };
        overlapping[1].shapes[0] = (prism_liquid_glass_shape){
            .bounds = {30.0f, 24.0f, 16.0f, 16.0f},
            .corner_radius = 2.0f,
        };
        bool overlap_seen[TEST_FRAME_SLOTS] = {false};
        for (uint32_t i = 0; i < TEST_FRAME_SLOTS; ++i) {
            uint32_t slot = render_liquid_glass_frame(s, canvas, target, blur_filter,
                                                      persistent_filter, overlapping, 2u, px);
            if (slot < TEST_FRAME_SLOTS)
                overlap_seen[slot] = true;
            const uint8_t *first_only = &px[(32u * W + 28u) * 4u];
            const uint8_t *second_centre = &px[(32u * W + 38u) * 4u];
            EXPECT(first_only[0] > 10u || first_only[1] > 10u || first_only[2] > 10u);
            EXPECT(second_centre[0] > 10u || second_centre[1] > 10u || second_centre[2] > 10u);
        }

        /* A later body's shadow is source-over material, not a replacement
         * for an earlier body. This mirrors a popover whose shadow footprint
         * reaches a Dock below it: the Dock must remain visible underneath
         * the translucent shadow instead of becoming a black cut-out. */
        prism_liquid_glass_group shadow_overlap[2] = {
            PRISM_LIQUID_GLASS_GROUP_INIT,
            PRISM_LIQUID_GLASS_GROUP_INIT,
        };
        shadow_overlap[0].shapes[0] = (prism_liquid_glass_shape){
            .bounds = {8.0f, 36.0f, 48.0f, 20.0f},
            .corner_radius = 6.0f,
        };
        shadow_overlap[1].shapes[0] = (prism_liquid_glass_shape){
            .bounds = {16.0f, 8.0f, 32.0f, 20.0f},
            .corner_radius = 6.0f,
        };
        shadow_overlap[1].shadow_alpha = 0.7f;
        shadow_overlap[1].shadow_blur = 6.0f;
        shadow_overlap[1].shadow_offset_y = 6.0f;
        for (uint32_t i = 0; i < TEST_FRAME_SLOTS; ++i) {
            render_liquid_glass_frame(s, canvas, target, blur_filter, persistent_filter,
                                      shadow_overlap, 2u, px);
            const uint8_t *dock_under_shadow = &px[(40u * W + 32u) * 4u];
            EXPECT(dock_under_shadow[0] > 10u || dock_under_shadow[1] > 10u ||
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
            EXPECT(old_first[0] < 5u && old_first[1] < 5u && old_first[2] < 5u);
            EXPECT(old_second[0] < 5u && old_second[1] < 5u && old_second[2] < 5u);
        }
        EXPECT(cleared_reused_slot);
        prism_liquid_glass_filter_release(persistent_filter);
    }

    flux_device_wait_idle(d);
    prism_liquid_glass_filter_release(glass_filter);
    flux_blur_filter_release(blur_filter);
    flux_image_release(target);
    flux_canvas_destroy(canvas);
    flux_surface_release(s);
    flux_device_release(d);
    TEST_SUMMARY();
}
