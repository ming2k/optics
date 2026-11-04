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
#include <flux/observe.h>
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
        EXPECT(
            flux_canvas_begin(canvas, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                               .frame = frame,
                                                               .clear_color = &clear}) == FLUX_OK);
        draw_cover(canvas, image);
        EXPECT(flux_canvas_end(canvas) == FLUX_OK);
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

    /* Drain the frames in flight so the surface slot releases the last
     * tracked foreign image (image 399) retained by frame_retain_image. */
    for (uint32_t f = 0; f < 3u; ++f) {
        flux_frame *df = nullptr;
        EXPECT(flux_surface_begin_frame(surface, nullptr, &df) == FLUX_OK);
        EXPECT(flux_frame_submit(df) == FLUX_OK);
        EXPECT(flux_frame_present(df) == FLUX_OK);
    }
    flux_device_wait_idle(d);

    /* Release-without-frame: zombies parked with NO recording frame must
     * be reclaimed immediately by wait_idle (ADR-0139, recording_frames). */
    flux_image *stray = make_cover(d, 0xFF112233u);
    EXPECT(stray != nullptr);
    flux_image_release(stray);
    flux_device_wait_idle(d);

    flux_observe_counters c = {};
    EXPECT(flux_observe_read_counters(d, FLUX_SCOPE_DEVICE, "vk_image", "flux/core", &c) ==
           FLUX_OK);
    /* At this point only the surface's swapchain/offscreen images remain live. */
    uint64_t baseline_live = c.live;

    /* Regression test: recording_frames counter integrity across aborted frames.
     *
     * Case A: flux_surface_resize during recording must decrement recording_frames
     * so wait_idle remains a functional reclamation point. */
    {
        flux_frame *rec_frame = nullptr;
        EXPECT(flux_surface_begin_frame(surface, nullptr, &rec_frame) == FLUX_OK);

        flux_image *zombie_a = make_cover(d, 0xAABBCCDDu);
        EXPECT(zombie_a != nullptr);
        flux_image_release(zombie_a);

        /* Abort the recording frame by resizing the surface. */
        EXPECT(flux_surface_resize(surface, SURF_W * 2, SURF_H * 2) == FLUX_OK);

        /* If recording_frames leaked, zombie_a would be tagged (submit_serial + 1)
         * and wait_idle would not reclaim it. */
        flux_device_wait_idle(d);
        EXPECT(flux_observe_read_counters(d, FLUX_SCOPE_DEVICE, "vk_image", "flux/core", &c) ==
               FLUX_OK);
        EXPECT(c.live == baseline_live);
    }

    /* Case B: releasing a surface while a frame is recording must not leak
     * recording_frames either. */
    {
        flux_surface *surf_b = nullptr;
        flux_surface_desc desc = FLUX_SURFACE_DESC_INIT;
        desc.width = SURF_W;
        desc.height = SURF_H;
        EXPECT(flux_surface_create(d, &desc, &surf_b) == FLUX_OK);

        flux_frame *rec_b = nullptr;
        EXPECT(flux_surface_begin_frame(surf_b, nullptr, &rec_b) == FLUX_OK);

        flux_surface_release(surf_b);

        /* Device must have 0 recording frames now. Releasing an image and
         * calling wait_idle must reclaim it immediately. */
        flux_image *zombie_b = make_cover(d, 0x11223344u);
        EXPECT(zombie_b != nullptr);
        flux_image_release(zombie_b);
        flux_device_wait_idle(d);

        EXPECT(flux_observe_read_counters(d, FLUX_SCOPE_DEVICE, "vk_image", "flux/core", &c) ==
               FLUX_OK);
        EXPECT(c.live == baseline_live);
    }

    flux_canvas_release(canvas);
    flux_surface_release(surface);
    flux_device_release(d);
    TEST_SUMMARY();
}
