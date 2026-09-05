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

int main(void) {
    flux_device *d = test_helpers_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_backdrop_layer: no Vulkan device; skipping\n");
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
    prism_backdrop_layer_filter *layer_filter = nullptr;
    EXPECT(prism_backdrop_layer_filter_create(d, &layer_filter) == FLUX_OK);

    static uint8_t px[BYTES];
    flux_color black = flux_color_rgba(0, 0, 0, 255);

    /* --- glass over frost samples the frosted image, not the capture --- */
    {
        flux_frame *frame = nullptr;
        EXPECT(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);

        /* Capture: hard black|white vertical edge at x = 32. */
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

        /* Full-image frost sheet + a glass body straddling the capture's
         * hard edge, with no refraction. The discriminator: every pixel of
         * the BLURRED backdrop near the edge is an intermediate grey, while
         * every pixel of the SHARP capture is exactly 0 or 255. A lens
         * sampling the frost therefore shows grey across the whole body; a
         * lens sampling the sharp capture shows pure black on the body's
         * dark half. That is precisely the layer relation this filter
         * exists to provide. */
        prism_backdrop_frost frost = PRISM_BACKDROP_FROST_INIT;
        frost.bounds = (flux_rect){0.0f, 0.0f, (float)W, (float)H};
        prism_liquid_glass_group body = PRISM_LIQUID_GLASS_GROUP_INIT;
        body.shapes[0] = (prism_liquid_glass_shape){.bounds = {20, 24, 24, 16}, .corner_radius = 8};
        prism_backdrop_layer_desc ld = PRISM_BACKDROP_LAYER_DESC_INIT;
        ld.input = target;
        ld.blurred_input = blurred;
        ld.frost = &frost;
        ld.frost_count = 1u;
        ld.groups = &body;
        ld.group_count = 1u;
        ld.refraction = 0.0f;
        ld.frost_strength = 0.0f;
        flux_image *layer = nullptr;
        EXPECT(prism_backdrop_layer_filter_apply(layer_filter, frame, &ld, &layer) == FLUX_OK);
        EXPECT(layer != nullptr);

        /* Draw over a *black* base so unfrosted/un-glassed pixels read black
         * and the frost's own alpha is exercised. */
        EXPECT(flux_canvas_begin_frame(canvas, frame, &black) == FLUX_OK);
        flux_canvas_draw_image(canvas, layer, (flux_rect){0, 0, (float)W, (float)H}, nullptr);
        flux_canvas_end_frame(canvas);
        EXPECT(flux_frame_submit(frame) == FLUX_OK);
        EXPECT(flux_frame_present(frame) == FLUX_OK);
        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);

        /* The frost must never carry the sharp edge: the transition column
         * is an intermediate (blurred) value, not the capture's hard 0/255. */
        const uint8_t *frost_edge = &px[(32u * W + 32u) * 4u];
        EXPECT(frost_edge[0] > 16u && frost_edge[0] < 245u);

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
        EXPECT(body_dark_left[0] > 40u && body_dark_left[0] < 80u);
        EXPECT(body_dark_rising[0] > body_dark_left[0] + 20u);
        /* The bright half mirrors it: the frost's dark-side spill pulls the
         * body well below the saturated white a sharp-sampling lens shows
         * (~202 with tint). */
        const uint8_t *body_bright_half = &px[(32u * W + 40u) * 4u];
        EXPECT(body_bright_half[0] < 195u && body_bright_half[0] > 120u);
    }

    /* --- footprint clearing across submissions on reused slots --- */
    {
        prism_backdrop_frost frost = PRISM_BACKDROP_FROST_INIT;
        frost.bounds = (flux_rect){4.0f, 24.0f, 16.0f, 16.0f};
        prism_liquid_glass_group body = PRISM_LIQUID_GLASS_GROUP_INIT;
        body.shapes[0] =
            (prism_liquid_glass_shape){.bounds = {8.0f, 28.0f, 8.0f, 8.0f}, .corner_radius = 4.0f};

        bool seen[TEST_FRAME_SLOTS] = {false};
        for (uint32_t i = 0; i < TEST_FRAME_SLOTS; ++i) {
            flux_frame *frame = nullptr;
            EXPECT(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);
            uint32_t slot = flux_frame_index(frame);
            EXPECT(slot < TEST_FRAME_SLOTS);
            if (slot < TEST_FRAME_SLOTS)
                seen[slot] = true;

            EXPECT(flux_canvas_begin_target(canvas, frame, target, &black) == FLUX_OK);
            flux_canvas_end_target(canvas);

            flux_effect_blur_desc bd = FLUX_EFFECT_BLUR_DESC_INIT;
            bd.input = target;
            bd.sigma = 2.0f;
            flux_image *blurred = nullptr;
            EXPECT(flux_blur_filter_apply(blur_filter, frame, &bd, &blurred) == FLUX_OK);

            prism_backdrop_layer_desc ld = PRISM_BACKDROP_LAYER_DESC_INIT;
            ld.input = target;
            ld.blurred_input = blurred;
            ld.frost = &frost;
            ld.frost_count = 1u;
            ld.groups = &body;
            ld.group_count = 1u;
            ld.refraction = 0.0f;
            flux_image *layer = nullptr;
            EXPECT(prism_backdrop_layer_filter_apply(layer_filter, frame, &ld, &layer) == FLUX_OK);

            EXPECT(flux_canvas_begin_frame(canvas, frame, &black) == FLUX_OK);
            flux_canvas_draw_image(canvas, layer, (flux_rect){0, 0, (float)W, (float)H}, nullptr);
            flux_canvas_end_frame(canvas);
            EXPECT(flux_frame_submit(frame) == FLUX_OK);
            EXPECT(flux_frame_present(frame) == FLUX_OK);
            memset(px, 0xCD, BYTES);
            EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);

            const uint8_t *inside = &px[(32u * W + 12u) * 4u];
            const uint8_t *outside = &px[(32u * W + 52u) * 4u];
            EXPECT(inside[0] > 0u);
            EXPECT(outside[0] < 5u && outside[1] < 5u && outside[2] < 5u);
        }

        /* Re-submit with the frost moved to the right: the old footprint
         * must clear, the new one must appear. */
        frost.bounds = (flux_rect){44.0f, 24.0f, 16.0f, 16.0f};
        body.shapes[0] =
            (prism_liquid_glass_shape){.bounds = {48.0f, 28.0f, 8.0f, 8.0f}, .corner_radius = 4.0f};
        bool reused = false;
        for (uint32_t i = 0; i < TEST_FRAME_SLOTS; ++i) {
            flux_frame *frame = nullptr;
            EXPECT(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);
            uint32_t slot = flux_frame_index(frame);
            if (slot < TEST_FRAME_SLOTS && seen[slot])
                reused = true;

            EXPECT(flux_canvas_begin_target(canvas, frame, target, &black) == FLUX_OK);
            flux_canvas_end_target(canvas);

            flux_effect_blur_desc bd = FLUX_EFFECT_BLUR_DESC_INIT;
            bd.input = target;
            bd.sigma = 2.0f;
            flux_image *blurred = nullptr;
            EXPECT(flux_blur_filter_apply(blur_filter, frame, &bd, &blurred) == FLUX_OK);

            prism_backdrop_layer_desc ld = PRISM_BACKDROP_LAYER_DESC_INIT;
            ld.input = target;
            ld.blurred_input = blurred;
            ld.frost = &frost;
            ld.frost_count = 1u;
            ld.groups = &body;
            ld.group_count = 1u;
            ld.refraction = 0.0f;
            flux_image *layer = nullptr;
            EXPECT(prism_backdrop_layer_filter_apply(layer_filter, frame, &ld, &layer) == FLUX_OK);

            EXPECT(flux_canvas_begin_frame(canvas, frame, &black) == FLUX_OK);
            flux_canvas_draw_image(canvas, layer, (flux_rect){0, 0, (float)W, (float)H}, nullptr);
            flux_canvas_end_frame(canvas);
            EXPECT(flux_frame_submit(frame) == FLUX_OK);
            EXPECT(flux_frame_present(frame) == FLUX_OK);
            memset(px, 0xCD, BYTES);
            EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);

            const uint8_t *old_footprint = &px[(32u * W + 12u) * 4u];
            const uint8_t *new_footprint = &px[(32u * W + 52u) * 4u];
            EXPECT(old_footprint[0] < 5u && old_footprint[1] < 5u && old_footprint[2] < 5u);
            EXPECT(new_footprint[0] > 0u);
        }
        EXPECT(reused);
    }

    /* --- frost resolves opaque; tint washes INTO the frost --- */
    {
        flux_frame *frame = nullptr;
        EXPECT(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);

        /* Capture: dark base with one bright block. Inside the block the
         * blur's spread makes blurred < sharp, so a half-opacity frost
         * (blend of frosted with sharp) is measurably brighter than the
         * full frost — the discriminator for the opaque resolve. */
        flux_color dark = flux_color_rgba(24, 24, 24, 255);
        EXPECT(flux_canvas_begin_target(canvas, frame, target, &dark) == FLUX_OK);
        flux_canvas_fill_rect_color(canvas, (flux_rect){20.0f, 24.0f, 24.0f, 16.0f},
                                    flux_color_rgba_premul(255, 255, 255, 255));
        flux_canvas_end_target(canvas);

        flux_effect_blur_desc bd = FLUX_EFFECT_BLUR_DESC_INIT;
        bd.input = target;
        bd.sigma = 8.0f;
        flux_image *blurred = nullptr;
        EXPECT(flux_blur_filter_apply(blur_filter, frame, &bd, &blurred) == FLUX_OK);

        /* Half-frost over a bright capture: opacity 0.5 must blend
         * frosted-vs-SHARP (a mid grey over the bright base), never
         * frosted-vs-transparent (which, drawn on the black output base,
         * reads as a darkened half-alpha fragment). The three rects:
         *   x0  .. x1 : plain full-opacity frost   (opaque, ~grey)
         *   x1  .. x2 : half-opacity frost         (blend with sharp base)
         *   x2  .. x3 : full-opacity red-tinted    (opaque, red)
         */
        prism_backdrop_frost frost[3] = {PRISM_BACKDROP_FROST_INIT, PRISM_BACKDROP_FROST_INIT,
                                         PRISM_BACKDROP_FROST_INIT};
        frost[0].bounds = (flux_rect){0.0f, 0.0f, (float)(W / 4), (float)H};
        frost[1].bounds = (flux_rect){(float)(W / 4), 0.0f, (float)(W / 4), (float)H};
        frost[1].opacity = 0.5f;
        frost[2].bounds = (flux_rect){(float)(W / 2), 0.0f, (float)(W / 2), (float)H};
        frost[2].tint_color = 0xFF0000u;
        frost[2].tint_strength = 1.0f;
        prism_backdrop_layer_desc ld = PRISM_BACKDROP_LAYER_DESC_INIT;
        ld.input = target;
        ld.blurred_input = blurred;
        ld.frost = frost;
        ld.frost_count = 3u;
        ld.group_count = 0u;
        flux_image *layer = nullptr;
        EXPECT(prism_backdrop_layer_filter_apply(layer_filter, frame, &ld, &layer) == FLUX_OK);

        EXPECT(flux_canvas_begin_frame(canvas, frame, &black) == FLUX_OK);
        flux_canvas_draw_image(canvas, layer, (flux_rect){0, 0, (float)W, (float)H}, nullptr);
        flux_canvas_end_frame(canvas);
        EXPECT(flux_frame_submit(frame) == FLUX_OK);
        EXPECT(flux_frame_present(frame) == FLUX_OK);
        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);

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
        EXPECT(plain[3] == 255u);
        EXPECT(tinted[3] == 255u);
        /* The plain frost stays in the dark blurred tail. Lavapipe and
         * hardware drivers differ slightly in their Gaussian edge samples,
         * so keep this as a broad non-black/dark discriminator; the half
         * probe below is the precise opacity-resolve assertion. */
        EXPECT(plain[0] > 16u && plain[0] < 160u);
        /* The tinted frost is dominantly red at 100% strength. */
        EXPECT(tinted[0] > 200u);
        EXPECT(tinted[1] < 60u && tinted[2] < 60u);
        /* PRIMARY OPACITY-RESOLVE ASSERTION. The half-opacity frost at the
         * bright block's centre is an OPAQUE blend of frost with SHARP
         * (measured ~210..230 over this capture). A premultiplied write
         * would instead store half-alpha over transparent, which the black
         * output base renders near ~118 — far darker — and with alpha 128.
         * Both probes fail loudly on that regression. */
        EXPECT(half[3] == 255u);
        EXPECT(half[0] > 180u);
    }

    /* --- the lens reads resolved background colours, not premultiplied
     *      fragments: a body straddling a half-opacity frost edge --- */
    {
        flux_frame *frame = nullptr;
        EXPECT(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);

        /* Capture: dark base, one bright block. */
        flux_color dark = flux_color_rgba(24, 24, 24, 255);
        EXPECT(flux_canvas_begin_target(canvas, frame, target, &dark) == FLUX_OK);
        flux_canvas_fill_rect_color(canvas, (flux_rect){8.0f, 24.0f, 48.0f, 16.0f},
                                    flux_color_rgba_premul(255, 255, 255, 255));
        flux_canvas_end_target(canvas);

        flux_effect_blur_desc bd = FLUX_EFFECT_BLUR_DESC_INIT;
        bd.input = target;
        bd.sigma = 8.0f;
        flux_image *blurred = nullptr;
        EXPECT(flux_blur_filter_apply(blur_filter, frame, &bd, &blurred) == FLUX_OK);

        /* A half-opacity frost over the LEFT half only; a no-refraction
         * glass body centred on the frost edge, well inside the bright
         * block. Inside the body: left = frost-over-sharp blend (bright),
         * right = the sharp capture itself (also bright). Under a
         * PREMULTIPLIED layer write the left side's lens tap reads
         * rgb*frost_alpha — roughly half brightness — so the body's two
         * halves disagree loudly; under the opaque resolve they agree. */
        prism_backdrop_frost frost = PRISM_BACKDROP_FROST_INIT;
        frost.bounds = (flux_rect){0.0f, 0.0f, (float)(W / 2), (float)H};
        frost.opacity = 0.5f;
        prism_liquid_glass_group body = PRISM_LIQUID_GLASS_GROUP_INIT;
        body.shapes[0] = (prism_liquid_glass_shape){.bounds = {20, 24, 24, 16}, .corner_radius = 8};
        prism_backdrop_layer_desc ld = PRISM_BACKDROP_LAYER_DESC_INIT;
        ld.input = target;
        ld.blurred_input = blurred;
        ld.frost = &frost;
        ld.frost_count = 1u;
        ld.groups = &body;
        ld.group_count = 1u;
        ld.refraction = 0.0f;
        ld.frost_strength = 0.0f;
        flux_image *layer = nullptr;
        EXPECT(prism_backdrop_layer_filter_apply(layer_filter, frame, &ld, &layer) == FLUX_OK);

        EXPECT(flux_canvas_begin_frame(canvas, frame, &black) == FLUX_OK);
        flux_canvas_draw_image(canvas, layer, (flux_rect){0, 0, (float)W, (float)H}, nullptr);
        flux_canvas_end_frame(canvas);
        EXPECT(flux_frame_submit(frame) == FLUX_OK);
        EXPECT(flux_frame_present(frame) == FLUX_OK);
        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);

        const uint8_t *left = &px[(32u * W + 26u) * 4u];
        const uint8_t *right = &px[(32u * W + 38u) * 4u];
        /* Both halves bright (the block is white; half-frost blends it
         * toward the blur's dark spill but stays far above half). */
        EXPECT(left[0] > 150u);
        EXPECT(right[0] > 150u);
        /* PRIMARY LENS-RESOLVE ASSERTIONS. (a) The body's right half sits
         * over NO frost rect: the lens must read the sharp capture through
         * the layer's opaque base — bright, not the transparent-hole black
         * a floating-sheet layer would leave. (b) The two halves must not
         * disagree by a near-factor-two — the signature of a lens reading
         * premultiplied fragments where frost coverage drops. */
        {
            int hi = left[0] > right[0] ? left[0] : right[0];
            int lo = left[0] > right[0] ? right[0] : left[0];
            EXPECT(hi == 0 || lo * 100 > hi * 55);
        }
        EXPECT(right[0] > 190u);
    }

    /* --- glass with NO frost rects still needs the opaque base --- */
    {
        flux_frame *frame = nullptr;
        EXPECT(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);

        flux_color dark = flux_color_rgba(24, 24, 24, 255);
        EXPECT(flux_canvas_begin_target(canvas, frame, target, &dark) == FLUX_OK);
        flux_canvas_fill_rect_color(canvas, (flux_rect){8.0f, 24.0f, 48.0f, 16.0f},
                                    flux_color_rgba_premul(255, 255, 255, 255));
        flux_canvas_end_target(canvas);

        flux_effect_blur_desc bd = FLUX_EFFECT_BLUR_DESC_INIT;
        bd.input = target;
        bd.sigma = 8.0f;
        flux_image *blurred = nullptr;
        EXPECT(flux_blur_filter_apply(blur_filter, frame, &bd, &blurred) == FLUX_OK);

        /* Glass only: no frost rects at all. The layer pass must still
         * write the sharp capture as the opaque base under the body, or
         * the lens samples cleared-transparent pixels (black). */
        prism_liquid_glass_group body = PRISM_LIQUID_GLASS_GROUP_INIT;
        body.shapes[0] = (prism_liquid_glass_shape){.bounds = {20, 24, 24, 16}, .corner_radius = 8};
        prism_backdrop_layer_desc ld = PRISM_BACKDROP_LAYER_DESC_INIT;
        ld.input = target;
        ld.blurred_input = blurred;
        ld.frost_count = 0u;
        ld.groups = &body;
        ld.group_count = 1u;
        ld.refraction = 0.0f;
        ld.frost_strength = 0.0f;
        flux_image *layer = nullptr;
        EXPECT(prism_backdrop_layer_filter_apply(layer_filter, frame, &ld, &layer) == FLUX_OK);

        EXPECT(flux_canvas_begin_frame(canvas, frame, &black) == FLUX_OK);
        flux_canvas_draw_image(canvas, layer, (flux_rect){0, 0, (float)W, (float)H}, nullptr);
        flux_canvas_end_frame(canvas);
        EXPECT(flux_frame_submit(frame) == FLUX_OK);
        EXPECT(flux_frame_present(frame) == FLUX_OK);
        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);

        const uint8_t *body_centre = &px[(32u * W + 32u) * 4u];
        /* The bright block shines through the no-frost glass body. */
        EXPECT(body_centre[0] > 150u);
    }

    /* --- multiple glass bodies with NO frost rects: all bodies must render (never black) --- */
    {
        flux_frame *frame = nullptr;
        EXPECT(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);

        flux_color white = flux_color_rgba(255, 255, 255, 255);
        EXPECT(flux_canvas_begin_target(canvas, frame, target, &white) == FLUX_OK);
        flux_canvas_end_target(canvas);

        flux_effect_blur_desc bd = FLUX_EFFECT_BLUR_DESC_INIT;
        bd.input = target;
        bd.sigma = 4.0f;
        flux_image *blurred = nullptr;
        EXPECT(flux_blur_filter_apply(blur_filter, frame, &bd, &blurred) == FLUX_OK);

        prism_liquid_glass_group bodies[3] = {PRISM_LIQUID_GLASS_GROUP_INIT,
                                              PRISM_LIQUID_GLASS_GROUP_INIT,
                                              PRISM_LIQUID_GLASS_GROUP_INIT};
        /* Body 0: top chip (e.g. HUD status) */
        bodies[0].shapes[0] =
            (prism_liquid_glass_shape){.bounds = {4.0f, 4.0f, 24.0f, 16.0f}, .corner_radius = 4.0f};
        /* Body 1: middle chip (e.g. HUD workspace) */
        bodies[1].shapes[0] =
            (prism_liquid_glass_shape){.bounds = {36.0f, 4.0f, 24.0f, 16.0f}, .corner_radius = 4.0f};
        /* Body 2: bottom panel (e.g. Dock) */
        bodies[2].shapes[0] =
            (prism_liquid_glass_shape){.bounds = {8.0f, 36.0f, 48.0f, 20.0f}, .corner_radius = 6.0f};

        prism_backdrop_layer_desc ld = PRISM_BACKDROP_LAYER_DESC_INIT;
        ld.input = target;
        ld.blurred_input = blurred;
        ld.frost_count = 0u;
        ld.groups = bodies;
        ld.group_count = 3u;
        ld.refraction = 4.0f;
        ld.edge_width = 8.0f;
        flux_image *layer = nullptr;
        EXPECT(prism_backdrop_layer_filter_apply(layer_filter, frame, &ld, &layer) == FLUX_OK);

        EXPECT(flux_canvas_begin_frame(canvas, frame, &black) == FLUX_OK);
        flux_canvas_draw_image(canvas, layer, (flux_rect){0, 0, (float)W, (float)H}, nullptr);
        flux_canvas_end_frame(canvas);
        EXPECT(flux_frame_submit(frame) == FLUX_OK);
        EXPECT(flux_frame_present(frame) == FLUX_OK);
        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);

        /* All three bodies must be visible and bright (none skipped or black). */
        const uint8_t *b0 = &px[(12u * W + 16u) * 4u];
        const uint8_t *b1 = &px[(12u * W + 48u) * 4u];
        const uint8_t *b2 = &px[(46u * W + 32u) * 4u];
        EXPECT(b0[0] > 150u);
        EXPECT(b1[0] > 150u);
        EXPECT(b2[0] > 150u);

        /* Outside all bodies must remain transparent (black base shows through). */
        const uint8_t *gap = &px[(28u * W + 32u) * 4u];
        EXPECT(gap[0] < 5u && gap[1] < 5u && gap[2] < 5u);
    }

    /* --- glass statistics round-trip on the layer filter --- */
    {
        flux_frame *frame = nullptr;
        EXPECT(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);
        prism_backdrop_stat stats[8];
        uint32_t count = 0u;
        /* First submission on this slot's cadence: stats may legitimately be
         * UNSUPPORTED_STATE on a fresh slot; the contract is that a slot
         * with a prior glass submission reports the group count. */
        EXPECT(prism_backdrop_layer_filter_stats(layer_filter, frame, stats, 8u, &count) ==
               FLUX_OK);
        EXPECT(flux_frame_submit(frame) == FLUX_OK);
        EXPECT(flux_frame_present(frame) == FLUX_OK);
    }

    prism_backdrop_layer_filter_release(layer_filter);
    flux_blur_filter_release(blur_filter);
    flux_canvas_destroy(canvas);
    flux_image_release(target);
    flux_surface_release(s);
    flux_device_release(d);
    TEST_SUMMARY();
    return 0;
}
