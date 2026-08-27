/*
 * flux_image retire queue: an image released right after a frame that
 * sampled it must survive until the GPU provably passes that batch.
 * The pre-fix behaviour destroyed view/image/memory inline, which an
 * in-flight batch could still reference (i915: GPU hang -> context
 * reset -> VK_ERROR_DEVICE_LOST). This test churns
 * create -> draw -> release every frame so any inline destruction or
 * missing slot recycling trips an error on real hardware. Skips if no
 * Vulkan device.
 */
#include "test_helpers.h"
#include <flux/flux.h>
#include <flux/vulkan.h>

#include <string.h>

#define SURF_W 64u
#define SURF_H 64u
#define IMG_W 16u
#define IMG_H 16u
#define CHURN_FRAMES 400u

static flux_image *make_cover(flux_device *d, uint32_t rgba) {
    uint32_t pixels[IMG_W * IMG_H];
    for (uint32_t i = 0; i < IMG_W * IMG_H; ++i)
        pixels[i] = rgba;
    flux_image_desc desc = FLUX_IMAGE_DESC_INIT;
    desc.width = IMG_W;
    desc.height = IMG_H;
    desc.format = FLUX_FORMAT_RGBA8_UNORM;
    desc.initial_data = pixels;
    flux_image *out = nullptr;
    return flux_image_create(d, &desc, &out) == FLUX_OK ? out : nullptr;
}

static void draw_cover(flux_canvas *canvas, void *user) {
    flux_canvas_draw_image(canvas, user, (flux_rect){0, 0, SURF_W, SURF_H}, nullptr);
}

int main(void) {
    flux_device *d = test_helpers_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_image_retire: no Vulkan device; skipping\n");
        TEST_SUMMARY();
    }

    flux_surface *surface = nullptr;
    {
        flux_surface_desc desc = FLUX_SURFACE_DESC_INIT;
        desc.width = SURF_W;
        desc.height = SURF_H;
        EXPECT(flux_surface_create(d, &desc, &surface) == FLUX_OK);
    }
    flux_canvas *canvas = nullptr;
    {
        flux_canvas_desc desc = FLUX_CANVAS_DESC_INIT;
        desc.surface = surface;
        EXPECT(flux_canvas_create(&desc, &canvas) == FLUX_OK);
    }

    /* Churn: each frame samples a fresh image and releases the previous
     * one. Deferred destruction must keep the in-flight references
     * valid AND recycle bindless slots quickly enough that registration
     * never fails. */
    flux_image *previous = nullptr;
    uint32_t released = 0;
    for (uint32_t i = 0; i < CHURN_FRAMES; ++i) {
        flux_image *image = make_cover(d, 0xFF000000u | (i * 9973u));
        EXPECT(image != nullptr);
        flux_frame *frame = nullptr;
        EXPECT(flux_surface_begin_frame(surface, nullptr, &frame) == FLUX_OK);
        flux_color clear = flux_color_rgba(0, 0, 0, 255);
        EXPECT(flux_canvas_begin_frame(canvas, frame, &clear) == FLUX_OK);
        draw_cover(canvas, image);
        flux_canvas_end_frame(canvas);
        EXPECT(flux_frame_submit(frame) == FLUX_OK);
        EXPECT(flux_frame_present(frame) == FLUX_OK);
        if (previous) {
            flux_image_release(previous);
            ++released;
        }
        previous = image;
    }
    if (previous) {
        flux_image_release(previous);
        ++released;
    }
    EXPECT(released == CHURN_FRAMES);

    /* Release-without-frame: zombies parked after the last submission
     * must be destroyed by device teardown (drain path). */
    flux_image *stray = make_cover(d, 0xFF112233u);
    EXPECT(stray != nullptr);
    flux_image_release(stray);

    flux_canvas_destroy(canvas);
    flux_surface_release(surface);
    flux_device_release(d);
    TEST_SUMMARY();
}
