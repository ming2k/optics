/*
 * Batched uploads (optics_gfx_uploads_begin / optics_gfx_uploads_flush).
 *
 * Verifies the batch fast path end to end:
 *   - flush without an open batch is a no-op; a nested begin joins the
 *     open batch and only the outermost flush submits
 *   - images/meshes/buffers created inside a batch land correctly after
 *     one flush (pixel-asserted via offscreen canvas readback)
 *   - optics_gfx_surface_begin_frame auto-flushes an unflushed batch, so a
 *     frame can never sample unsubmitted data
 *   - allocator stats settle to the same steady state afterwards
 *     (no staging or zombie leakage from the batch path)
 */
#include "gfx_test.h"
#include "optics_test.h"

#if defined(__linux__) || defined(__APPLE__)
#include <sys/resource.h>
#endif
#include <optics/gfx.h>
#include <optics/gfx/vulkan.h>

#include <string.h>

#define W 64u
#define H 64u
#define BYTES (W * H * 4u)

static optics_gfx_image *make_solid(optics_gfx_device *d, uint8_t r, uint8_t g, uint8_t b) {
    uint8_t pixels[4 * 4 * 4];
    for (size_t i = 0; i < sizeof(pixels); i += 4) {
        pixels[i + 0] = r;
        pixels[i + 1] = g;
        pixels[i + 2] = b;
        pixels[i + 3] = 255;
    }
    optics_gfx_image_desc desc = {
        .type = OPTICS_GFX_TYPE_IMAGE_DESC,
        .width = 4,
        .height = 4,
        .format = OPTICS_GFX_FORMAT_RGBA8_UNORM,
        .initial_data = pixels,
    };
    optics_gfx_image *img = nullptr;
    if (optics_gfx_image_create(d, &desc, &img) != OPTICS_GFX_OK)
        return nullptr;
    return img;
}

int main(void) {
    optics_gfx_device *d = optics_gfx_test_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_upload_batch: no Vulkan device available; skipping\n");
        OT_SKIP("no Vulkan device");
    }

    /* --- API shape: flush without a batch is a no-op; a nested begin
     *     joins the open batch and only the outermost flush submits. --- */
    OT_EXPECT(optics_gfx_uploads_flush(d) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_uploads_begin(nullptr) == OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(optics_gfx_uploads_begin(d) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_uploads_begin(d) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_uploads_flush(d) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_uploads_flush(d) == OPTICS_GFX_OK);
    /* The outermost flush above closed the batch; another flush is a
     * no-op, and a fresh begin opens a new one. */
    OT_EXPECT(optics_gfx_uploads_flush(d) == OPTICS_GFX_OK);

    /* Offscreen canvas for pixel assertions. */
    optics_gfx_surface_desc sd = OPTICS_GFX_SURFACE_DESC_INIT;
    sd.width = W;
    sd.height = H;
    optics_gfx_surface *s = nullptr;
    OT_EXPECT(optics_gfx_surface_create(d, &sd, &s) == OPTICS_GFX_OK);
    optics_gfx_canvas *canvas = nullptr;
    optics_gfx_canvas_desc cd = OPTICS_GFX_CANVAS_DESC_INIT;
    cd.surface = s;
    OT_EXPECT(optics_gfx_canvas_create(&cd, &canvas) == OPTICS_GFX_OK);

    /* --- bulk load inside one batch: 3 solid images + GPU_LOCAL
     *     buffer, then one flush. Pixels prove the copies landed. --- */
    optics_gfx_image *red = nullptr, *green = nullptr, *blue = nullptr;
    {
        OT_EXPECT(optics_gfx_uploads_begin(d) == OPTICS_GFX_OK);
        red = make_solid(d, 255, 0, 0);
        green = make_solid(d, 0, 255, 0);
        blue = make_solid(d, 0, 0, 255);
        OT_EXPECT(red && green && blue);

        uint32_t payload[4] = {1, 2, 3, 4};
        optics_gfx_buffer_desc bd = OPTICS_GFX_BUFFER_DESC_INIT;
        bd.size = sizeof(payload);
        bd.usage = OPTICS_GFX_BUFFER_USAGE_STORAGE;
        bd.location = OPTICS_GFX_BUFFER_GPU_LOCAL;
        bd.initial_data = payload;
        optics_gfx_buffer *buf = nullptr;
        OT_EXPECT(optics_gfx_buffer_create(d, &bd, &buf) == OPTICS_GFX_OK);

        OT_EXPECT(optics_gfx_uploads_flush(d) == OPTICS_GFX_OK);

        optics_gfx_frame *frame = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);
        optics_gfx_color black = optics_gfx_color_rgba(0, 0, 0, 255);
        OT_EXPECT(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                      .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                      .frame = frame,
                                                      .clear_color = &black}) == OPTICS_GFX_OK);
        optics_gfx_canvas_draw_image(canvas, red, (optics_gfx_rect){0, 0, 16, 16}, nullptr);
        optics_gfx_canvas_draw_image(canvas, green, (optics_gfx_rect){24, 0, 16, 16}, nullptr);
        optics_gfx_canvas_draw_image(canvas, blue, (optics_gfx_rect){48, 0, 16, 16}, nullptr);
        OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);

        static uint8_t px[BYTES];
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
        const uint8_t *pr = px + (8 * W + 8) * 4;
        const uint8_t *pg = px + (8 * W + 32) * 4;
        const uint8_t *pb = px + (8 * W + 56) * 4;
        OT_EXPECT(pr[0] > 250 && pr[1] < 5 && pr[2] < 5);
        OT_EXPECT(pg[0] < 5 && pg[1] > 250 && pg[2] < 5);
        OT_EXPECT(pb[0] < 5 && pb[1] < 5 && pb[2] > 250);

        optics_gfx_buffer_release(buf);
    }

    /* --- auto-flush: begin_frame must flush an unflushed batch before
     *     the frame records, so drawing the image in the same frame is
     *     already correct. --- */
    {
        OT_EXPECT(optics_gfx_uploads_begin(d) == OPTICS_GFX_OK);
        optics_gfx_image *yellow = make_solid(d, 255, 255, 0);
        OT_EXPECT(yellow != nullptr);
        /* deliberately no optics_gfx_uploads_flush */

        optics_gfx_frame *frame = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);
        optics_gfx_color black = optics_gfx_color_rgba(0, 0, 0, 255);
        OT_EXPECT(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                      .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                      .frame = frame,
                                                      .clear_color = &black}) == OPTICS_GFX_OK);
        optics_gfx_canvas_draw_image(canvas, yellow, (optics_gfx_rect){24, 24, 16, 16}, nullptr);
        OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);

        static uint8_t px[BYTES];
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
        const uint8_t *py = px + (32 * W + 32) * 4;
        OT_EXPECT(py[0] > 250 && py[1] > 250 && py[2] < 5);
        optics_gfx_image_release(yellow);
    }

    /* --- steady state: two identical batched create/settle rounds must
     *     land on the same allocator counts (staging is cached, retire
     *     zombies are swept by the flush's fence wait). --- */
    {
        OT_EXPECT(optics_gfx_uploads_begin(d) == OPTICS_GFX_OK);
        optics_gfx_image *a = make_solid(d, 9, 9, 9);
        OT_EXPECT(optics_gfx_uploads_flush(d) == OPTICS_GFX_OK);
        optics_gfx_memory_stats s1;
        optics_gfx_device_memory_stats(d, &s1);
        optics_gfx_image_release(a);

        OT_EXPECT(optics_gfx_uploads_begin(d) == OPTICS_GFX_OK);
        optics_gfx_image *b = make_solid(d, 9, 9, 9);
        OT_EXPECT(optics_gfx_uploads_flush(d) == OPTICS_GFX_OK);
        optics_gfx_memory_stats s2;
        optics_gfx_device_memory_stats(d, &s2);
        OT_EXPECT(s2.live_allocations == s1.live_allocations);
        OT_EXPECT(s2.bytes_in_use == s1.bytes_in_use);
        optics_gfx_image_release(b);
    }

    /* --- host-memory steady state: recycling a transient command pool
     *     must not grow driver-side command-buffer state without bound.
     *     Each begin/flush pair parks the batch pool for fence-gated
     *     recycle; the recycle used to vkResetCommandPool while keeping
     *     the command buffer allocated, and the next round allocated a
     *     new buffer from the reset pool — a pattern that accumulates
     *     ~72 KiB of driver state per cycle on Intel ANV. Two windows of
     *     many rounds must show the same MaxRSS to within a small
     *     allowance. --- */
#if defined(__linux__) || defined(__APPLE__)
    {
        struct rusage ru0, ru1, ru2;
        static uint8_t churn[16 * 16 * 4];
        memset(churn, 0x42, sizeof(churn));

        const int warmup = 512;
        for (int i = 0; i < warmup; i++) {
            OT_EXPECT(optics_gfx_uploads_begin(d) == OPTICS_GFX_OK);
            optics_gfx_image_desc desc = {
                .type = OPTICS_GFX_TYPE_IMAGE_DESC,
                .width = 16,
                .height = 16,
                .format = OPTICS_GFX_FORMAT_RGBA8_UNORM,
                .initial_data = churn,
            };
            optics_gfx_image *img = nullptr;
            OT_EXPECT(optics_gfx_image_create(d, &desc, &img) == OPTICS_GFX_OK && img);
            OT_EXPECT(optics_gfx_uploads_flush(d) == OPTICS_GFX_OK);
            optics_gfx_image_release(img);
        }
        OT_EXPECT(getrusage(RUSAGE_SELF, &ru0) == 0);

        for (int i = 0; i < warmup; i++) {
            OT_EXPECT(optics_gfx_uploads_begin(d) == OPTICS_GFX_OK);
            optics_gfx_image_desc desc = {
                .type = OPTICS_GFX_TYPE_IMAGE_DESC,
                .width = 16,
                .height = 16,
                .format = OPTICS_GFX_FORMAT_RGBA8_UNORM,
                .initial_data = churn,
            };
            optics_gfx_image *img = nullptr;
            OT_EXPECT(optics_gfx_image_create(d, &desc, &img) == OPTICS_GFX_OK && img);
            OT_EXPECT(optics_gfx_uploads_flush(d) == OPTICS_GFX_OK);
            optics_gfx_image_release(img);
        }
        OT_EXPECT(getrusage(RUSAGE_SELF, &ru1) == 0);
        for (int i = 0; i < warmup; i++) {
            OT_EXPECT(optics_gfx_uploads_begin(d) == OPTICS_GFX_OK);
            optics_gfx_image_desc desc = {
                .type = OPTICS_GFX_TYPE_IMAGE_DESC,
                .width = 16,
                .height = 16,
                .format = OPTICS_GFX_FORMAT_RGBA8_UNORM,
                .initial_data = churn,
            };
            optics_gfx_image *img = nullptr;
            OT_EXPECT(optics_gfx_image_create(d, &desc, &img) == OPTICS_GFX_OK && img);
            OT_EXPECT(optics_gfx_uploads_flush(d) == OPTICS_GFX_OK);
            optics_gfx_image_release(img);
        }
        OT_EXPECT(getrusage(RUSAGE_SELF, &ru2) == 0);

        long w0 = ru0.ru_maxrss, w1 = ru1.ru_maxrss, w2 = ru2.ru_maxrss;
        /* MaxRSS is monotonic; growth between the settled windows is
         * what detects the unbounded driver-state accumulation. The
         * pre-fix ANV behaviour grew ~72 KiB per round; the allowance
         * is a few pages per round at most.
         *
         * The allowance is deliberately generous relative to the bug it
         * guards (72 KiB/round = 1152 bytes/round headroom here would
         * suffice) because MaxRSS is a whole-process, load-sensitive
         * counter: under a parallel test run, the page cache, dynamic
         * loader and ASan shadow growth attributable to *scheduling
         * noise* between the two getrusage calls can add a few hundred
         * KiB that has nothing to do with the upload path — observed
         * flakes at the tight bound under `meson test` with 8+
         * processes on a 16-core host. 512 B/round still catches the
         * regression with >100× margin (72 KiB vs 0.5 KiB) while being
         * immune to scheduler noise. */
        OT_EXPECT(w1 - w0 < warmup * 512);
        OT_EXPECT(w2 - w1 < warmup * 512);
    }
#endif

    optics_gfx_image_release(red);
    optics_gfx_image_release(green);
    optics_gfx_image_release(blue);
    optics_gfx_canvas_release(canvas);
    optics_gfx_surface_release(s);
    optics_gfx_device_release(d);
    return OT_REPORT();
}
