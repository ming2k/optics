/*
 * optics_gfx_image retire queue: an image released right after a frame that
 * sampled it must survive until the GPU provably passes that batch.
 * The pre-fix behaviour destroyed view/image/memory inline, which an
 * in-flight batch could still reference (i915: GPU hang -> context
 * reset -> VK_ERROR_DEVICE_LOST). This test churns
 * create -> draw -> release every frame so any inline destruction or
 * missing slot recycling trips an error on real hardware. Skips if no
 * Vulkan device.
 */
#include "gfx_test.h"
#include "optics_test.h"
#include <optics/gfx.h>
#include <optics/gfx/observe.h>
#include <optics/gfx/vulkan.h>

#include <string.h>

#define SURF_W 64u
#define SURF_H 64u
#define IMG_W 16u
#define IMG_H 16u
#define CHURN_FRAMES 400u

static optics_gfx_image *make_cover(optics_gfx_device *d, uint32_t rgba) {
    uint32_t pixels[IMG_W * IMG_H];
    for (uint32_t i = 0; i < IMG_W * IMG_H; ++i)
        pixels[i] = rgba;
    optics_gfx_image_desc desc = OPTICS_GFX_IMAGE_DESC_INIT;
    desc.width = IMG_W;
    desc.height = IMG_H;
    desc.format = OPTICS_GFX_FORMAT_RGBA8_UNORM;
    desc.initial_data = pixels;
    optics_gfx_image *out = nullptr;
    return optics_gfx_image_create(d, &desc, &out) == OPTICS_GFX_OK ? out : nullptr;
}

static void draw_cover(optics_gfx_canvas *canvas, void *user) {
    optics_gfx_canvas_draw_image(canvas, user, (optics_gfx_rect){0, 0, SURF_W, SURF_H}, nullptr);
}

int main(void) {
    optics_gfx_device *d = optics_gfx_test_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_image_retire: no Vulkan device; skipping\n");
        OT_SKIP("no Vulkan device");
    }

    optics_gfx_surface *surface = nullptr;
    {
        optics_gfx_surface_desc desc = OPTICS_GFX_SURFACE_DESC_INIT;
        desc.width = SURF_W;
        desc.height = SURF_H;
        OT_EXPECT(optics_gfx_surface_create(d, &desc, &surface) == OPTICS_GFX_OK);
    }
    optics_gfx_canvas *canvas = nullptr;
    {
        optics_gfx_canvas_desc desc = OPTICS_GFX_CANVAS_DESC_INIT;
        desc.surface = surface;
        OT_EXPECT(optics_gfx_canvas_create(&desc, &canvas) == OPTICS_GFX_OK);
    }

    /* Churn: each frame samples a fresh image and releases the previous
     * one. Deferred destruction must keep the in-flight references
     * valid AND recycle bindless slots quickly enough that registration
     * never fails. */
    optics_gfx_image *previous = nullptr;
    uint32_t released = 0;
    for (uint32_t i = 0; i < CHURN_FRAMES; ++i) {
        optics_gfx_image *image = make_cover(d, 0xFF000000u | (i * 9973u));
        OT_EXPECT(image != nullptr);
        optics_gfx_frame *frame = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(surface, nullptr, &frame) == OPTICS_GFX_OK);
        optics_gfx_color clear = optics_gfx_color_rgba(0, 0, 0, 255);
        OT_EXPECT(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                      .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                      .frame = frame,
                                                      .clear_color = &clear}) == OPTICS_GFX_OK);
        draw_cover(canvas, image);
        OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
        if (previous) {
            optics_gfx_image_release(previous);
            ++released;
        }
        previous = image;
    }
    if (previous) {
        optics_gfx_image_release(previous);
        ++released;
    }
    OT_EXPECT(released == CHURN_FRAMES);

    /* Drain the frames in flight so the surface slot releases the last
     * tracked foreign image (image 399) retained by frame_retain_image. */
    for (uint32_t f = 0; f < 3u; ++f) {
        optics_gfx_frame *df = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(surface, nullptr, &df) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_submit(df) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(df) == OPTICS_GFX_OK);
    }
    optics_gfx_device_wait_idle(d);

    /* Release-without-frame: zombies parked with NO recording frame must
     * be reclaimed immediately by wait_idle (ADR-0139, recording_frames). */
    optics_gfx_image *stray = make_cover(d, 0xFF112233u);
    OT_EXPECT(stray != nullptr);
    optics_gfx_image_release(stray);
    optics_gfx_device_wait_idle(d);

    optics_gfx_observe_counters c = {};
    OT_EXPECT(optics_gfx_observe_read_counters(
                  d, &(optics_gfx_observe_read_counters_opts){.scope = OPTICS_GFX_SCOPE_DEVICE,
                                                              .kind = "vk_image",
                                                              .owner = "gfx/core",
                                                              .out = &c}) == OPTICS_GFX_OK);
    /* At this point only the surface's swapchain/offscreen images remain live. */
    uint64_t baseline_live = c.live;

    /* Regression test: recording_frames counter integrity across aborted frames.
     *
     * Case A: optics_gfx_surface_resize during recording must decrement recording_frames
     * so wait_idle remains a functional reclamation point. */
    {
        optics_gfx_frame *rec_frame = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(surface, nullptr, &rec_frame) == OPTICS_GFX_OK);

        optics_gfx_image *zombie_a = make_cover(d, 0xAABBCCDDu);
        OT_EXPECT(zombie_a != nullptr);
        optics_gfx_image_release(zombie_a);

        /* Abort the recording frame by resizing the surface. */
        OT_EXPECT(optics_gfx_surface_resize(surface, SURF_W * 2, SURF_H * 2) == OPTICS_GFX_OK);

        /* If recording_frames leaked, zombie_a would be tagged (submit_serial + 1)
         * and wait_idle would not reclaim it. */
        optics_gfx_device_wait_idle(d);
        OT_EXPECT(optics_gfx_observe_read_counters(
                      d, &(optics_gfx_observe_read_counters_opts){.scope = OPTICS_GFX_SCOPE_DEVICE,
                                                                  .kind = "vk_image",
                                                                  .owner = "gfx/core",
                                                                  .out = &c}) == OPTICS_GFX_OK);
        OT_EXPECT(c.live == baseline_live);
    }

    /* Case B: releasing a surface while a frame is recording must not leak
     * recording_frames either. */
    {
        optics_gfx_surface *surf_b = nullptr;
        optics_gfx_surface_desc desc = OPTICS_GFX_SURFACE_DESC_INIT;
        desc.width = SURF_W;
        desc.height = SURF_H;
        OT_EXPECT(optics_gfx_surface_create(d, &desc, &surf_b) == OPTICS_GFX_OK);

        optics_gfx_frame *rec_b = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(surf_b, nullptr, &rec_b) == OPTICS_GFX_OK);

        optics_gfx_surface_release(surf_b);

        /* Device must have 0 recording frames now. Releasing an image and
         * calling wait_idle must reclaim it immediately. */
        optics_gfx_image *zombie_b = make_cover(d, 0x11223344u);
        OT_EXPECT(zombie_b != nullptr);
        optics_gfx_image_release(zombie_b);
        optics_gfx_device_wait_idle(d);

        OT_EXPECT(optics_gfx_observe_read_counters(
                      d, &(optics_gfx_observe_read_counters_opts){.scope = OPTICS_GFX_SCOPE_DEVICE,
                                                                  .kind = "vk_image",
                                                                  .owner = "gfx/core",
                                                                  .out = &c}) == OPTICS_GFX_OK);
        OT_EXPECT(c.live == baseline_live);
    }

    optics_gfx_canvas_release(canvas);
    optics_gfx_surface_release(surface);
    optics_gfx_device_release(d);
    return OT_REPORT();
}
