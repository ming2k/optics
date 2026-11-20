/*
 * Layered backdrop compositor (prism): frosted rectangles carrying analytic
 * liquid-glass bodies, composed in one dispatch into one transparent image.
 *
 * This is the pixel gate for the layer relation itself: a glass body must
 * sample the *frosted* image, not the sharp capture. The probe is a hard
 * black/white vertical edge in the capture:
 *
 *   - the frost rect (drawn from the blurred capture) never shows the sharp
 *     edge: every frosted pixel is an intermediate value;
 *   - a glass body over the frost with refraction 0 shows the frosted
 *     interior too — if the lens were sampling the sharp capture (the
 *     pre-fix standalone behaviour), the body's centre would follow the
 *     sharp edge instead of the frost;
 *   - outside the frost rect and every glass silhouette the output stays
 *     exactly transparent.
 *
 * Also asserts the persistent per-slot output clears previous + current
 * footprints across submissions, mirroring test_liquid_glass.c.
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

static optics_gfx_result apply_layer(optics_material_backdrop_layer_filter *filter,
                                     optics_gfx_frame *frame,
                                     const optics_material_backdrop_layer_desc *desc,
                                     optics_material_output *out) {
    return optics_material_backdrop_layer_filter_apply(
        filter, &(optics_material_backdrop_layer_filter_apply_opts){
                    .frame = frame, .desc = desc, .out = out});
}

static optics_gfx_result stats_layer(optics_material_backdrop_layer_filter *filter,
                                     optics_gfx_frame *frame, optics_material_backdrop_stat *out,
                                     uint32_t max_groups, uint32_t *out_count) {
    return optics_material_backdrop_layer_filter_stats(
        filter, &(optics_material_backdrop_layer_filter_stats_opts){
                    .frame = frame, .out = out, .max_groups = max_groups, .out_count = out_count});
}
#define TEST_FRAME_SLOTS 3u /* mirrors FLUX_MAX_FRAMES_IN_FLIGHT */

int main(void) {
    optics_gfx_device *d = optics_gfx_test_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_backdrop_layer: no Vulkan device; skipping\n");
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
    optics_material_backdrop_layer_filter *layer_filter = nullptr;
    OT_EXPECT(optics_material_backdrop_layer_filter_create(d, &layer_filter) == OPTICS_GFX_OK);
    OT_EXPECT(optics_material_backdrop_layer_filter_warmup(nullptr, nullptr) ==
              OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(optics_material_backdrop_layer_filter_warmup(layer_filter, nullptr) == OPTICS_GFX_OK);

    static uint8_t px[BYTES];
    optics_gfx_color black = optics_gfx_color_rgba(0, 0, 0, 255);

    /* --- glass over frost samples the frosted image, not the capture --- */
    {
        optics_gfx_frame *frame = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);

        /* Capture: hard black|white vertical edge at x = 32. */
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

        /* Full-image frost sheet + a glass body straddling the capture's
         * hard edge, with no refraction. The discriminator: every pixel of
         * the BLURRED backdrop near the edge is an intermediate grey, while
         * every pixel of the SHARP capture is exactly 0 or 255. A lens
         * sampling the frost therefore shows grey across the whole body; a
         * lens sampling the sharp capture shows pure black on the body's
         * dark half. That is precisely the layer relation this filter
         * exists to provide. */
        optics_material_base_material frost = OPTICS_MATERIAL_BASE_MATERIAL_INIT;
        frost.bounds = (optics_gfx_rect){0.0f, 0.0f, (float)W, (float)H};
        optics_material_liquid_glass_shape body_shapes[1] = {
            {.bounds = {20, 24, 24, 16}, .corner_radius = 8},
        };
        optics_material_liquid_glass_group body = OPTICS_MATERIAL_LIQUID_GLASS_GROUP_INIT;
        body.shapes = body_shapes;
        body.shape_count = 1;
        optics_material_backdrop_layer_desc ld = OPTICS_MATERIAL_BACKDROP_LAYER_DESC_INIT;
        ld.input = target;
        ld.blurred_input = blurred;
        ld.base_materials = &frost;
        ld.base_material_count = 1u;
        ld.groups = &body;
        ld.group_count = 1u;
        ld.refraction = 0.0f;
        ld.frost_strength = 0.0f;
        optics_material_output layer = {};
        OT_EXPECT(apply_layer(layer_filter, frame, &ld, &layer) == OPTICS_GFX_OK);
        OT_EXPECT(layer.image != nullptr);

        /* Draw over a *black* base so unfrosted/un-glassed pixels read black
         * and the frost's own alpha is exercised. */
        OT_EXPECT(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                      .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                      .frame = frame,
                                                      .clear_color = &black}) == OPTICS_GFX_OK);
        test_draw_material(canvas, layer);
        OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);

        /* The frost must never carry the sharp edge: the transition column
         * is an intermediate (blurred) value, not the capture's hard 0/255. */
        const uint8_t *frost_edge = &px[(32u * W + 32u) * 4u];
        OT_EXPECT(frost_edge[0] > 16u && frost_edge[0] < 245u);

        /* PRIMARY LAYER ASSERTION. The body spans x=20..44 across the edge.
         * On the dark half (x=22..30) the frost shows its bright-side spill
         * as a rising gradient (measured ~43 -> ~110 at this sigma), while
         * a lens sampling the SHARP capture sees uniform black and renders
         * only the material's tint floor (~26). Two probes pin the gradient
         * itself, not a single lifted pixel: the body's left edge must sit
         * in the 40..80 band and rise by more than 20 across 4 pixels — a
         * flat tint floor cannot pass either. */
        const uint8_t *body_dark_left = &px[(32u * W + 22u) * 4u];
        const uint8_t *body_dark_rising = &px[(32u * W + 26u) * 4u];
        OT_EXPECT(body_dark_left[0] > 40u && body_dark_left[0] < 80u);
        OT_EXPECT(body_dark_rising[0] > body_dark_left[0] + 20u);
        /* The bright half mirrors it: the frost's dark-side spill pulls the
         * body well below the saturated white a sharp-sampling lens shows
         * (~202 with tint). */
        const uint8_t *body_bright_half = &px[(32u * W + 40u) * 4u];
        OT_EXPECT(body_bright_half[0] < 240u && body_bright_half[0] > 120u);
    }

    /* --- footprint clearing across submissions on reused slots --- */
    {
        optics_material_base_material frost = OPTICS_MATERIAL_BASE_MATERIAL_INIT;
        frost.bounds = (optics_gfx_rect){4.0f, 24.0f, 16.0f, 16.0f};
        optics_material_liquid_glass_shape body_shapes[1] = {
            {.bounds = {8.0f, 28.0f, 8.0f, 8.0f}, .corner_radius = 4.0f},
        };
        optics_material_liquid_glass_group body = OPTICS_MATERIAL_LIQUID_GLASS_GROUP_INIT;
        body.shapes = body_shapes;
        body.shape_count = 1;

        bool seen[TEST_FRAME_SLOTS] = {false};
        for (uint32_t i = 0; i < TEST_FRAME_SLOTS; ++i) {
            optics_gfx_frame *frame = nullptr;
            OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);
            uint32_t slot = optics_gfx_frame_index(frame);
            OT_EXPECT(slot < TEST_FRAME_SLOTS);
            if (slot < TEST_FRAME_SLOTS)
                seen[slot] = true;

            OT_EXPECT(optics_gfx_canvas_begin(
                          canvas, &(optics_gfx_canvas_pass_desc){
                                      .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                      .frame = frame,
                                      .attachment = {.kind = OPTICS_GFX_CANVAS_ATTACHMENT_IMAGE,
                                                     .image = target},
                                      .clear_color = &black}) == OPTICS_GFX_OK);
            OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);

            optics_gfx_effect_blur_desc bd = OPTICS_GFX_EFFECT_BLUR_DESC_INIT;
            bd.input = target;
            bd.sigma = 2.0f;
            optics_gfx_image *blurred = nullptr;
            OT_EXPECT(optics_gfx_blur_filter_apply(
                          blur_filter,
                          &(optics_gfx_blur_filter_apply_opts){
                              .frame = frame, .desc = &bd, .out = &blurred}) == OPTICS_GFX_OK);

            optics_material_backdrop_layer_desc ld = OPTICS_MATERIAL_BACKDROP_LAYER_DESC_INIT;
            ld.input = target;
            ld.blurred_input = blurred;
            ld.base_materials = &frost;
            ld.base_material_count = 1u;
            ld.groups = &body;
            ld.group_count = 1u;
            ld.refraction = 0.0f;
            optics_material_output layer = {};
            OT_EXPECT(apply_layer(layer_filter, frame, &ld, &layer) == OPTICS_GFX_OK);

            OT_EXPECT(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                          .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                          .frame = frame,
                                                          .clear_color = &black}) == OPTICS_GFX_OK);
            test_draw_material(canvas, layer);
            OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
            OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
            OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
            memset(px, 0xCD, BYTES);
            OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);

            const uint8_t *inside = &px[(32u * W + 12u) * 4u];
            const uint8_t *outside = &px[(32u * W + 52u) * 4u];
            OT_EXPECT(inside[0] > 0u);
            OT_EXPECT(outside[0] < 5u && outside[1] < 5u && outside[2] < 5u);
        }

        /* Re-submit with the frost moved to the right: the old footprint
         * must clear, the new one must appear. */
        frost.bounds = (optics_gfx_rect){44.0f, 24.0f, 16.0f, 16.0f};
        body_shapes[0] = (optics_material_liquid_glass_shape){.bounds = {48.0f, 28.0f, 8.0f, 8.0f},
                                                              .corner_radius = 4.0f};
        bool reused = false;
        for (uint32_t i = 0; i < TEST_FRAME_SLOTS; ++i) {
            optics_gfx_frame *frame = nullptr;
            OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);
            uint32_t slot = optics_gfx_frame_index(frame);
            if (slot < TEST_FRAME_SLOTS && seen[slot])
                reused = true;

            OT_EXPECT(optics_gfx_canvas_begin(
                          canvas, &(optics_gfx_canvas_pass_desc){
                                      .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                      .frame = frame,
                                      .attachment = {.kind = OPTICS_GFX_CANVAS_ATTACHMENT_IMAGE,
                                                     .image = target},
                                      .clear_color = &black}) == OPTICS_GFX_OK);
            OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);

            optics_gfx_effect_blur_desc bd = OPTICS_GFX_EFFECT_BLUR_DESC_INIT;
            bd.input = target;
            bd.sigma = 2.0f;
            optics_gfx_image *blurred = nullptr;
            OT_EXPECT(optics_gfx_blur_filter_apply(
                          blur_filter,
                          &(optics_gfx_blur_filter_apply_opts){
                              .frame = frame, .desc = &bd, .out = &blurred}) == OPTICS_GFX_OK);

            optics_material_backdrop_layer_desc ld = OPTICS_MATERIAL_BACKDROP_LAYER_DESC_INIT;
            ld.input = target;
            ld.blurred_input = blurred;
            ld.base_materials = &frost;
            ld.base_material_count = 1u;
            ld.groups = &body;
            ld.group_count = 1u;
            ld.refraction = 0.0f;
            optics_material_output layer = {};
            OT_EXPECT(apply_layer(layer_filter, frame, &ld, &layer) == OPTICS_GFX_OK);

            OT_EXPECT(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                          .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                          .frame = frame,
                                                          .clear_color = &black}) == OPTICS_GFX_OK);
            test_draw_material(canvas, layer);
            OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
            OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
            OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
            memset(px, 0xCD, BYTES);
            OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);

            const uint8_t *old_footprint = &px[(32u * W + 12u) * 4u];
            const uint8_t *new_footprint = &px[(32u * W + 52u) * 4u];
            OT_EXPECT(old_footprint[0] < 5u && old_footprint[1] < 5u && old_footprint[2] < 5u);
            OT_EXPECT(new_footprint[0] > 0u);
        }
        OT_EXPECT(reused);
    }

    /* --- frost resolves opaque; tint washes INTO the frost --- */
    {
        optics_gfx_frame *frame = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);

        /* Capture: dark base with one bright block. Inside the block the
         * blur's spread makes blurred < sharp, so a half-opacity frost
         * (blend of frosted with sharp) is measurably brighter than the
         * full frost — the discriminator for the opaque resolve. */
        optics_gfx_color dark = optics_gfx_color_rgba(24, 24, 24, 255);
        OT_EXPECT(optics_gfx_canvas_begin(
                      canvas, &(optics_gfx_canvas_pass_desc){
                                  .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                  .frame = frame,
                                  .attachment = {.kind = OPTICS_GFX_CANVAS_ATTACHMENT_IMAGE,
                                                 .image = target},
                                  .clear_color = &dark}) == OPTICS_GFX_OK);
        optics_gfx_canvas_fill_rect_color(canvas, (optics_gfx_rect){20.0f, 24.0f, 24.0f, 16.0f},
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

        /* Half-frost over a bright capture: opacity 0.5 must blend
         * frosted-vs-SHARP (a mid grey over the bright base), never
         * frosted-vs-transparent (which, drawn on the black output base,
         * reads as a darkened half-alpha fragment). The three rects:
         *   x0  .. x1 : plain full-opacity frost   (opaque, ~grey)
         *   x1  .. x2 : half-opacity frost         (blend with sharp base)
         *   x2  .. x3 : full-opacity red-tinted    (opaque, red)
         */
        optics_material_base_material frost[3] = {OPTICS_MATERIAL_BASE_MATERIAL_INIT,
                                                  OPTICS_MATERIAL_BASE_MATERIAL_INIT,
                                                  OPTICS_MATERIAL_BASE_MATERIAL_INIT};
        frost[0].bounds = (optics_gfx_rect){0.0f, 0.0f, (float)(W / 4), (float)H};
        frost[1].bounds = (optics_gfx_rect){(float)(W / 4), 0.0f, (float)(W / 4), (float)H};
        frost[1].opacity = 0.5f;
        frost[2].bounds = (optics_gfx_rect){(float)(W / 2), 0.0f, (float)(W / 2), (float)H};
        frost[2].tint_color = 0xFF0000u;
        frost[2].tint_strength = 1.0f;
        optics_material_backdrop_layer_desc ld = OPTICS_MATERIAL_BACKDROP_LAYER_DESC_INIT;
        ld.input = target;
        ld.blurred_input = blurred;
        ld.base_materials = frost;
        ld.base_material_count = 3u;
        ld.group_count = 0u;
        optics_material_output layer = {};
        OT_EXPECT(apply_layer(layer_filter, frame, &ld, &layer) == OPTICS_GFX_OK);

        OT_EXPECT(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                      .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                      .frame = frame,
                                                      .clear_color = &black}) == OPTICS_GFX_OK);
        test_draw_material(canvas, layer);
        OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);

        /* Probes sit inside the bright block (x=20..44, row 32). The
         * full-frost rect covers x=0..16 only, so `plain` is probed at the
         * block's left edge inside the HALF rect would conflate the two;
         * instead probe the full frost through its own coverage of the
         * block's blurred tail (x=12..16, dark side) and the half frost at
         * the block centre where blurred < sharp. */
        const uint8_t *plain = &px[(32u * W + 12u) * 4u];
        const uint8_t *half = &px[(32u * W + 26u) * 4u];
        const uint8_t *tinted = &px[(32u * W + 48u) * 4u];
        /* Full-opacity rects are opaque. */
        OT_EXPECT(plain[3] == 255u);
        OT_EXPECT(tinted[3] == 255u);
        /* The plain frost stays in the dark blurred tail. Lavapipe and
         * hardware drivers differ slightly in their Gaussian edge samples,
         * so keep this as a broad non-black/dark discriminator; the half
         * probe below is the precise opacity-resolve assertion. */
        OT_EXPECT(plain[0] > 16u && plain[0] < 160u);
        /* The tinted frost is dominantly red at 100% strength. */
        OT_EXPECT(tinted[0] > 200u);
        OT_EXPECT(tinted[1] < 60u && tinted[2] < 60u);
        /* PRIMARY OPACITY-RESOLVE ASSERTION. The half-opacity frost at the
         * bright block's centre is an OPAQUE blend of frost with SHARP
         * (measured ~210..230 over this capture). A premultiplied write
         * would instead store half-alpha over transparent, which the black
         * output base renders near ~118 — far darker — and with alpha 128.
         * Both probes fail loudly on that regression. */
        OT_EXPECT(half[3] == 255u);
        OT_EXPECT(half[0] > 180u);
    }

    /* --- the lens reads resolved background colours, not premultiplied
     *      fragments: a body straddling a half-opacity frost edge --- */
    {
        optics_gfx_frame *frame = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);

        /* Capture: dark base, one bright block. */
        optics_gfx_color dark = optics_gfx_color_rgba(24, 24, 24, 255);
        OT_EXPECT(optics_gfx_canvas_begin(
                      canvas, &(optics_gfx_canvas_pass_desc){
                                  .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                  .frame = frame,
                                  .attachment = {.kind = OPTICS_GFX_CANVAS_ATTACHMENT_IMAGE,
                                                 .image = target},
                                  .clear_color = &dark}) == OPTICS_GFX_OK);
        optics_gfx_canvas_fill_rect_color(canvas, (optics_gfx_rect){8.0f, 24.0f, 48.0f, 16.0f},
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

        /* A half-opacity frost over the LEFT half only; a no-refraction
         * glass body centred on the frost edge, well inside the bright
         * block. Inside the body: left = frost-over-sharp blend (bright),
         * right = the sharp capture itself (also bright). Under a
         * PREMULTIPLIED layer write the left side's lens tap reads
         * rgb*frost_alpha — roughly half brightness — so the body's two
         * halves disagree loudly; under the opaque resolve they agree. */
        optics_material_base_material frost = OPTICS_MATERIAL_BASE_MATERIAL_INIT;
        frost.bounds = (optics_gfx_rect){0.0f, 0.0f, (float)(W / 2), (float)H};
        frost.opacity = 0.5f;
        optics_material_liquid_glass_shape body_shapes[1] = {
            {.bounds = {20, 24, 24, 16}, .corner_radius = 8},
        };
        optics_material_liquid_glass_group body = OPTICS_MATERIAL_LIQUID_GLASS_GROUP_INIT;
        body.shapes = body_shapes;
        body.shape_count = 1;
        optics_material_backdrop_layer_desc ld = OPTICS_MATERIAL_BACKDROP_LAYER_DESC_INIT;
        ld.input = target;
        ld.blurred_input = blurred;
        ld.base_materials = &frost;
        ld.base_material_count = 1u;
        ld.groups = &body;
        ld.group_count = 1u;
        ld.refraction = 0.0f;
        ld.frost_strength = 0.0f;
        optics_material_output layer = {};
        OT_EXPECT(apply_layer(layer_filter, frame, &ld, &layer) == OPTICS_GFX_OK);

        OT_EXPECT(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                      .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                      .frame = frame,
                                                      .clear_color = &black}) == OPTICS_GFX_OK);
        test_draw_material(canvas, layer);
        OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);

        const uint8_t *left = &px[(32u * W + 26u) * 4u];
        const uint8_t *right = &px[(32u * W + 38u) * 4u];
        /* Both halves bright (the block is white; half-frost blends it
         * toward the blur's dark spill but stays far above half). */
        OT_EXPECT(left[0] > 150u);
        OT_EXPECT(right[0] > 150u);
        /* PRIMARY LENS-RESOLVE ASSERTIONS. (a) The body's right half sits
         * over NO frost rect: the lens must read the sharp capture through
         * the layer's opaque base — bright, not the transparent-hole black
         * a floating-sheet layer would leave. (b) The two halves must not
         * disagree by a near-factor-two — the signature of a lens reading
         * premultiplied fragments where frost coverage drops. */
        {
            int hi = left[0] > right[0] ? left[0] : right[0];
            int lo = left[0] > right[0] ? right[0] : left[0];
            OT_EXPECT(hi == 0 || lo * 100 > hi * 55);
        }
        OT_EXPECT(right[0] > 190u);
    }

    /* --- glass with NO frost rects still needs the opaque base --- */
    {
        optics_gfx_frame *frame = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);

        optics_gfx_color dark = optics_gfx_color_rgba(24, 24, 24, 255);
        OT_EXPECT(optics_gfx_canvas_begin(
                      canvas, &(optics_gfx_canvas_pass_desc){
                                  .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                  .frame = frame,
                                  .attachment = {.kind = OPTICS_GFX_CANVAS_ATTACHMENT_IMAGE,
                                                 .image = target},
                                  .clear_color = &dark}) == OPTICS_GFX_OK);
        optics_gfx_canvas_fill_rect_color(canvas, (optics_gfx_rect){8.0f, 24.0f, 48.0f, 16.0f},
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

        /* Glass only: no frost rects at all. The layer pass must still
         * write the sharp capture as the opaque base under the body, or
         * the lens samples cleared-transparent pixels (black). */
        optics_material_liquid_glass_shape body_shapes[1] = {
            {.bounds = {20, 24, 24, 16}, .corner_radius = 8},
        };
        optics_material_liquid_glass_group body = OPTICS_MATERIAL_LIQUID_GLASS_GROUP_INIT;
        body.shapes = body_shapes;
        body.shape_count = 1;
        optics_material_backdrop_layer_desc ld = OPTICS_MATERIAL_BACKDROP_LAYER_DESC_INIT;
        ld.input = target;
        ld.blurred_input = blurred;
        ld.base_material_count = 0u;
        ld.groups = &body;
        ld.group_count = 1u;
        ld.refraction = 0.0f;
        ld.frost_strength = 0.0f;
        optics_material_output layer = {};
        OT_EXPECT(apply_layer(layer_filter, frame, &ld, &layer) == OPTICS_GFX_OK);

        OT_EXPECT(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                      .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                      .frame = frame,
                                                      .clear_color = &black}) == OPTICS_GFX_OK);
        test_draw_material(canvas, layer);
        OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);

        const uint8_t *body_centre = &px[(32u * W + 32u) * 4u];
        /* The bright block shines through the no-frost glass body. */
        OT_EXPECT(body_centre[0] > 150u);
    }

    /* --- overlapping base-material plates compose in array order --- */
    {
        /* Two plates that overlap. Plate A carries a red wash, plate B keeps
         * the plain frosted backdrop. They are dispatched per merged region,
         * so both passes touch the shared pixels: the earlier plate must
         * survive everywhere outside the later plate's own silhouette.
         *
         * Before the base/plate stage split, every plate pass wrote an opaque
         * pixel for every dispatched pixel, so B's pass covered A's exclusive
         * area with the sharp backdrop — A vanished. That is invisible to any
         * single-plate test and to the layer-order test above (which only has
         * one plate per region), so it needs its own gate. */
        optics_gfx_frame *frame = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);

        optics_gfx_color white = optics_gfx_color_rgba(255, 255, 255, 255);
        OT_EXPECT(optics_gfx_canvas_begin(
                      canvas, &(optics_gfx_canvas_pass_desc){
                                  .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                  .frame = frame,
                                  .attachment = {.kind = OPTICS_GFX_CANVAS_ATTACHMENT_IMAGE,
                                                 .image = target},
                                  .clear_color = &white}) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);

        optics_gfx_effect_blur_desc bd = OPTICS_GFX_EFFECT_BLUR_DESC_INIT;
        bd.input = target;
        bd.sigma = 6.0f;
        optics_gfx_image *blurred = nullptr;
        OT_EXPECT(optics_gfx_blur_filter_apply(blur_filter,
                                               &(optics_gfx_blur_filter_apply_opts){
                                                   .frame = frame, .desc = &bd, .out = &blurred}) ==
                  OPTICS_GFX_OK);

        optics_material_base_material plates[2] = {OPTICS_MATERIAL_BASE_MATERIAL_INIT,
                                                   OPTICS_MATERIAL_BASE_MATERIAL_INIT};
        plates[0].bounds = (optics_gfx_rect){8.0f, 8.0f, 32.0f, 32.0f}; /* A: x 8..40 */
        plates[0].opacity = 1.0f;
        plates[0].tint_color = 0xFF0000u;
        plates[0].tint_strength = 1.0f;
        plates[1].bounds = (optics_gfx_rect){24.0f, 24.0f, 32.0f, 32.0f}; /* B: x 24..56 */
        plates[1].opacity = 1.0f;
        plates[1].tint_color = 0xFFFFFFu;
        plates[1].tint_strength = 0.0f;

        optics_material_backdrop_layer_desc ld = OPTICS_MATERIAL_BACKDROP_LAYER_DESC_INIT;
        ld.input = target;
        ld.blurred_input = blurred;
        ld.base_materials = plates;
        ld.base_material_count = 2u;
        ld.group_count = 0u;
        optics_material_output layer = {};
        OT_EXPECT(apply_layer(layer_filter, frame, &ld, &layer) == OPTICS_GFX_OK);

        OT_EXPECT(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                      .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                      .frame = frame,
                                                      .clear_color = &black}) == OPTICS_GFX_OK);
        test_draw_material(canvas, layer);
        OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);

        const uint8_t *a_left = &px[(12u * W + 12u) * 4u];  /* A only */
        const uint8_t *a_right = &px[(12u * W + 30u) * 4u]; /* A only, other side */
        const uint8_t *b_only = &px[(40u * W + 44u) * 4u];  /* B only */
        const uint8_t *a_and_b = &px[(28u * W + 28u) * 4u]; /* both */
        const uint8_t *outside = &px[(4u * W + 4u) * 4u];   /* neither */

        /* A's exclusive area keeps A's wash: red stays red. */
        OT_EXPECT(a_left[0] > 200u && a_left[1] < 60u && a_left[2] < 60u);
        OT_EXPECT(a_right[0] > 200u && a_right[1] < 60u && a_right[2] < 60u);
        /* B only and the overlap read the plain (untinted) plate. */
        OT_EXPECT(b_only[0] > 200u && b_only[1] > 200u && b_only[2] > 200u);
        OT_EXPECT(a_and_b[0] > 200u && a_and_b[1] > 200u && a_and_b[2] > 200u);
        /* Outside every plate the layer is transparent. */
        OT_EXPECT(outside[0] == 0u && outside[1] == 0u && outside[2] == 0u);
    }

    /* --- multiple glass bodies with NO frost rects: all bodies must render (never black) --- */
    {
        optics_gfx_frame *frame = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);

        optics_gfx_color white = optics_gfx_color_rgba(255, 255, 255, 255);
        OT_EXPECT(optics_gfx_canvas_begin(
                      canvas, &(optics_gfx_canvas_pass_desc){
                                  .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                  .frame = frame,
                                  .attachment = {.kind = OPTICS_GFX_CANVAS_ATTACHMENT_IMAGE,
                                                 .image = target},
                                  .clear_color = &white}) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);

        optics_gfx_effect_blur_desc bd = OPTICS_GFX_EFFECT_BLUR_DESC_INIT;
        bd.input = target;
        bd.sigma = 4.0f;
        optics_gfx_image *blurred = nullptr;
        OT_EXPECT(optics_gfx_blur_filter_apply(blur_filter,
                                               &(optics_gfx_blur_filter_apply_opts){
                                                   .frame = frame, .desc = &bd, .out = &blurred}) ==
                  OPTICS_GFX_OK);

        optics_material_liquid_glass_group bodies[3] = {OPTICS_MATERIAL_LIQUID_GLASS_GROUP_INIT,
                                                        OPTICS_MATERIAL_LIQUID_GLASS_GROUP_INIT,
                                                        OPTICS_MATERIAL_LIQUID_GLASS_GROUP_INIT};
        optics_material_liquid_glass_shape body_shapes[3][1] = {
            {{.bounds = {4.0f, 4.0f, 24.0f, 16.0f}, .corner_radius = 4.0f}},
            {{.bounds = {36.0f, 4.0f, 24.0f, 16.0f}, .corner_radius = 4.0f}},
            {{.bounds = {8.0f, 36.0f, 48.0f, 20.0f}, .corner_radius = 6.0f}},
        };
        for (size_t bi = 0; bi < 3; ++bi) {
            bodies[bi].shapes = body_shapes[bi];
            bodies[bi].shape_count = 1;
        }

        optics_material_backdrop_layer_desc ld = OPTICS_MATERIAL_BACKDROP_LAYER_DESC_INIT;
        ld.input = target;
        ld.blurred_input = blurred;
        ld.base_material_count = 0u;
        ld.groups = bodies;
        ld.group_count = 3u;
        ld.refraction = 4.0f;
        ld.edge_width = 8.0f;
        optics_material_output layer = {};
        OT_EXPECT(apply_layer(layer_filter, frame, &ld, &layer) == OPTICS_GFX_OK);

        OT_EXPECT(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                      .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                      .frame = frame,
                                                      .clear_color = &black}) == OPTICS_GFX_OK);
        test_draw_material(canvas, layer);
        OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);

        /* All three bodies must be visible and bright (none skipped or black). */
        const uint8_t *b0 = &px[(12u * W + 16u) * 4u];
        const uint8_t *b1 = &px[(12u * W + 48u) * 4u];
        const uint8_t *b2 = &px[(46u * W + 32u) * 4u];
        OT_EXPECT(b0[0] > 150u);
        OT_EXPECT(b1[0] > 150u);
        OT_EXPECT(b2[0] > 150u);

        /* Outside all bodies must remain transparent (black base shows through). */
        const uint8_t *gap = &px[(28u * W + 32u) * 4u];
        OT_EXPECT(gap[0] < 5u && gap[1] < 5u && gap[2] < 5u);
    }

    /* --- glass statistics round-trip on the layer filter --- */
    {
        optics_gfx_frame *frame = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);
        optics_material_backdrop_stat stats[8];
        uint32_t count = 0u;
        /* First submission on this slot's cadence: stats may legitimately be
         * UNSUPPORTED_STATE on a fresh slot; the contract is that a slot
         * with a prior glass submission reports the group count. */
        OT_EXPECT(stats_layer(layer_filter, frame, stats, 8u, &count) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
    }

    /* --- Material Backdrop Boundary Cache hit (ADR-0123 / INV-COMP-04) --- */
    {
        optics_gfx_frame *frame = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);
        optics_material_base_material frost = {
            .bounds = {16.0f, 16.0f, 32.0f, 32.0f},
            .corner_radius = 4.0f,
            .opacity = 1.0f,
            .tint_color = 0xFFFFFFu,
            .tint_strength = 0.0f,
        };
        optics_gfx_effect_blur_desc bd = OPTICS_GFX_EFFECT_BLUR_DESC_INIT;
        bd.input = target;
        bd.sigma = 4.0f;
        optics_gfx_image *blurred = nullptr;
        OT_EXPECT(optics_gfx_blur_filter_apply(blur_filter,
                                               &(optics_gfx_blur_filter_apply_opts){
                                                   .frame = frame, .desc = &bd, .out = &blurred}) ==
                  OPTICS_GFX_OK);

        optics_material_backdrop_layer_desc ld = OPTICS_MATERIAL_BACKDROP_LAYER_DESC_INIT;
        ld.input = target;
        ld.blurred_input = blurred;
        ld.base_materials = &frost;
        ld.base_material_count = 1u;
        /* Opt into the cache for this scenario. */
        optics_material_backdrop_layer_revision rev = OPTICS_MATERIAL_BACKDROP_LAYER_REVISION_INIT;
        rev.revision = 42u;
        ld.next = &rev;

        optics_material_output out1 = {};
        OT_EXPECT(apply_layer(layer_filter, frame, &ld, &out1) == OPTICS_GFX_OK);
        OT_EXPECT(out1.image != nullptr);

        /* Second application with the same revision on the same slot: hits the
         * cache and returns the exact same persistent image without any
         * dispatch. */
        optics_material_output out2 = {};
        OT_EXPECT(apply_layer(layer_filter, frame, &ld, &out2) == OPTICS_GFX_OK);
        OT_EXPECT(out2.image == out1.image);

        /* A new revision must invalidate, even though both images and the whole
         * descriptor are unchanged. This is the case the old handle/hash key
         * could not express: a repainted capture target and a pooled blur
         * output keep their handles, so content changes were invisible. */
        rev.revision = 43u;
        optics_material_output out3 = {};
        OT_EXPECT(apply_layer(layer_filter, frame, &ld, &out3) == OPTICS_GFX_OK);
        OT_EXPECT(out3.image == out1.image); /* same persistent image ... */
        /* ... re-rendered: the stats for this slot describe this submission,
         * which only happens on a cache miss. */

        /* With the cache disabled (revision 0) every call re-renders. */
        ld.next = nullptr;
        optics_material_output out4 = {};
        OT_EXPECT(apply_layer(layer_filter, frame, &ld, &out4) == OPTICS_GFX_OK);
        OT_EXPECT(out4.image == out1.image);

        /* An extension tag prism does not define is rejected instead of being
         * silently ignored: a dropped revision would quietly turn a
         * cache-enabled call into a stale-content call. */
        optics_material_backdrop_layer_revision bogus =
            OPTICS_MATERIAL_BACKDROP_LAYER_REVISION_INIT;
        bogus.type = (optics_material_struct_type)999;
        ld.next = &bogus;
        optics_material_output out5 = {};
        OT_EXPECT(apply_layer(layer_filter, frame, &ld, &out5) ==
                  OPTICS_GFX_ERROR_INVALID_ARGUMENT);
        OT_EXPECT(out5.image == nullptr);

        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
    }

    /* --- cache correctness: a revision bump must publish new backdrop content --- */
    {
        /* The layer holds one persistent output per slot and the glass lens
         * samples whatever the slot last produced. Gain is the observable: the
         * backdrop flips between black and white while every handle and every
         * descriptor field stays identical, so only the revision can carry the
         * change. */
        const optics_material_liquid_glass_shape body_shapes[1] = {
            {.bounds = {20.0f, 24.0f, 24.0f, 16.0f}, .corner_radius = 8.0f},
        };
        optics_material_liquid_glass_group body = OPTICS_MATERIAL_LIQUID_GLASS_GROUP_INIT;
        body.shapes = body_shapes;
        body.shape_count = 1;

        optics_material_backdrop_layer_revision rev = OPTICS_MATERIAL_BACKDROP_LAYER_REVISION_INIT;
        uint8_t levels[4] = {};
        for (uint32_t step = 0; step < 2; ++step) {
            uint8_t bg = step == 0 ? 16u : 240u;
            optics_gfx_frame *frame = nullptr;
            OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);

            optics_gfx_color backdrop = optics_gfx_color_rgba(bg, bg, bg, 255);
            OT_EXPECT(optics_gfx_canvas_begin(
                          canvas, &(optics_gfx_canvas_pass_desc){
                                      .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                      .frame = frame,
                                      .attachment = {.kind = OPTICS_GFX_CANVAS_ATTACHMENT_IMAGE,
                                                     .image = target},
                                      .clear_color = &backdrop}) == OPTICS_GFX_OK);
            OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);

            optics_gfx_effect_blur_desc bd = OPTICS_GFX_EFFECT_BLUR_DESC_INIT;
            bd.input = target;
            bd.sigma = 6.0f;
            optics_gfx_image *blurred = nullptr;
            OT_EXPECT(optics_gfx_blur_filter_apply(
                          blur_filter,
                          &(optics_gfx_blur_filter_apply_opts){
                              .frame = frame, .desc = &bd, .out = &blurred}) == OPTICS_GFX_OK);

            optics_material_base_material frost = OPTICS_MATERIAL_BASE_MATERIAL_INIT;
            frost.bounds = (optics_gfx_rect){0.0f, 0.0f, (float)W, (float)H};
            frost.opacity = 1.0f;

            optics_material_backdrop_layer_desc ld = OPTICS_MATERIAL_BACKDROP_LAYER_DESC_INIT;
            ld.input = target;
            ld.blurred_input = blurred;
            ld.base_materials = &frost;
            ld.base_material_count = 1u;
            ld.groups = &body;
            ld.group_count = 1u;
            ld.refraction = 0.0f;
            ld.frost_strength = 0.0f;
            ld.tint_strength = 0.0f;
            ld.next = &rev;

            optics_material_output layer = {};
            OT_EXPECT(apply_layer(layer_filter, frame, &ld, &layer) == OPTICS_GFX_OK);

            OT_EXPECT(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                          .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                          .frame = frame,
                                                          .clear_color = &black}) == OPTICS_GFX_OK);
            test_draw_material(canvas, layer);
            OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
            OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
            OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
            memset(px, 0xCD, BYTES);
            OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
            levels[step] = px[(32u * W + 32u) * 4u];
            rev.revision++;
        }
        /* Dark backdrop -> dark body; light backdrop -> light body. A frozen
         * cache would report the same level twice. */
        OT_EXPECT(levels[0] < 60u);
        OT_EXPECT(levels[1] > 180u);
    }

    optics_material_backdrop_layer_filter_release(layer_filter);
    optics_gfx_blur_filter_release(blur_filter);
    optics_gfx_canvas_release(canvas);
    optics_gfx_image_release(target);
    optics_gfx_surface_release(s);
    optics_gfx_device_release(d);
    return OT_REPORT();
}
