/*
 * Batched uploads (flux_uploads_begin / flux_uploads_flush).
 *
 * Verifies the batch fast path end to end:
 *   - flush without an open batch is a no-op; a nested begin joins the
 *     open batch and only the outermost flush submits
 *   - images/meshes/buffers created inside a batch land correctly after
 *     one flush (pixel-asserted via offscreen canvas readback)
 *   - flux_surface_begin_frame auto-flushes an unflushed batch, so a
 *     frame can never sample unsubmitted data
 *   - allocator stats settle to the same steady state afterwards
 *     (no staging or zombie leakage from the batch path)
 */
#include "test_helpers.h"
#include <flux/flux.h>
#include <flux/vulkan.h>

#include <string.h>
#include <sys/resource.h>

#define W 64u
#define H 64u
#define BYTES (W * H * 4u)

static flux_image *make_solid(flux_device *d, uint8_t r, uint8_t g, uint8_t b) {
    uint8_t pixels[4 * 4 * 4];
    for (size_t i = 0; i < sizeof(pixels); i += 4) {
        pixels[i + 0] = r;
        pixels[i + 1] = g;
        pixels[i + 2] = b;
        pixels[i + 3] = 255;
    }
    flux_image_desc desc = {
        .type = FLUX_TYPE_IMAGE_DESC,
        .width = 4,
        .height = 4,
        .format = FLUX_FORMAT_RGBA8_UNORM,
        .initial_data = pixels,
    };
    flux_image *img = NULL;
    if (flux_image_create(d, &desc, &img) != FLUX_OK)
        return NULL;
    return img;
}

int main(void) {
    flux_device *d = test_helpers_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_upload_batch: no Vulkan device available; skipping\n");
        return 0;
    }

    /* --- API shape: flush without a batch is a no-op; a nested begin
     *     joins the open batch and only the outermost flush submits. --- */
    EXPECT(flux_uploads_flush(d) == FLUX_OK);
    EXPECT(flux_uploads_begin(nullptr) == FLUX_ERROR_INVALID_ARGUMENT);
    EXPECT(flux_uploads_begin(d) == FLUX_OK);
    EXPECT(flux_uploads_begin(d) == FLUX_OK);
    EXPECT(flux_uploads_flush(d) == FLUX_OK);
    EXPECT(flux_uploads_flush(d) == FLUX_OK);
    /* The outermost flush above closed the batch; another flush is a
     * no-op, and a fresh begin opens a new one. */
    EXPECT(flux_uploads_flush(d) == FLUX_OK);

    /* Offscreen canvas for pixel assertions. */
    flux_surface_desc sd = FLUX_SURFACE_DESC_INIT;
    sd.width = W;
    sd.height = H;
    flux_surface *s = nullptr;
    EXPECT(flux_surface_create(d, &sd, &s) == FLUX_OK);
    flux_canvas *canvas = nullptr;
    flux_canvas_desc cd = FLUX_CANVAS_DESC_INIT;
    cd.surface = s;
    EXPECT(flux_canvas_create(&cd, &canvas) == FLUX_OK);

    /* --- bulk load inside one batch: 3 solid images + mesh + GPU_LOCAL
     *     buffer, then one flush. Pixels prove the copies landed. --- */
    flux_image *red = NULL, *green = NULL, *blue = NULL;
    {
        EXPECT(flux_uploads_begin(d) == FLUX_OK);
        red = make_solid(d, 255, 0, 0);
        green = make_solid(d, 0, 255, 0);
        blue = make_solid(d, 0, 0, 255);
        EXPECT(red && green && blue);

        flux_vertex verts[3] = {
            {{0, 0, 0}, {0, 0, 1}, {0, 0}},
            {{1, 0, 0}, {0, 0, 1}, {1, 0}},
            {{0, 1, 0}, {0, 0, 1}, {0, 1}},
        };
        flux_mesh_desc mdesc = {
            .type = FLUX_TYPE_MESH_DESC,
            .vertices = verts,
            .vertex_count = 3,
        };
        flux_mesh *mesh = NULL;
        EXPECT(flux_mesh_create(d, &mdesc, &mesh) == FLUX_OK);

        uint32_t payload[4] = {1, 2, 3, 4};
        flux_buffer_desc bd = FLUX_BUFFER_DESC_INIT;
        bd.size = sizeof(payload);
        bd.usage = FLUX_BUFFER_USAGE_STORAGE;
        bd.location = FLUX_BUFFER_GPU_LOCAL;
        bd.initial_data = payload;
        flux_buffer *buf = NULL;
        EXPECT(flux_buffer_create(d, &bd, &buf) == FLUX_OK);

        EXPECT(flux_uploads_flush(d) == FLUX_OK);

        flux_frame *frame = nullptr;
        EXPECT(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);
        flux_color black = flux_color_rgba(0, 0, 0, 255);
        EXPECT(flux_canvas_begin(canvas, frame, &black) == FLUX_OK);
        flux_canvas_draw_image(canvas, red, (flux_rect){0, 0, 16, 16}, NULL);
        flux_canvas_draw_image(canvas, green, (flux_rect){24, 0, 16, 16}, NULL);
        flux_canvas_draw_image(canvas, blue, (flux_rect){48, 0, 16, 16}, NULL);
        flux_canvas_end(canvas);
        EXPECT(flux_frame_submit(frame) == FLUX_OK);
        EXPECT(flux_frame_present(frame) == FLUX_OK);

        static uint8_t px[BYTES];
        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);
        const uint8_t *pr = px + (8 * W + 8) * 4;
        const uint8_t *pg = px + (8 * W + 32) * 4;
        const uint8_t *pb = px + (8 * W + 56) * 4;
        EXPECT(pr[0] > 250 && pr[1] < 5 && pr[2] < 5);
        EXPECT(pg[0] < 5 && pg[1] > 250 && pg[2] < 5);
        EXPECT(pb[0] < 5 && pb[1] < 5 && pb[2] > 250);

        flux_buffer_release(buf);
        flux_mesh_release(mesh);
    }

    /* --- auto-flush: begin_frame must flush an unflushed batch before
     *     the frame records, so drawing the image in the same frame is
     *     already correct. --- */
    {
        EXPECT(flux_uploads_begin(d) == FLUX_OK);
        flux_image *yellow = make_solid(d, 255, 255, 0);
        EXPECT(yellow != NULL);
        /* deliberately no flux_uploads_flush */

        flux_frame *frame = nullptr;
        EXPECT(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);
        flux_color black = flux_color_rgba(0, 0, 0, 255);
        EXPECT(flux_canvas_begin(canvas, frame, &black) == FLUX_OK);
        flux_canvas_draw_image(canvas, yellow, (flux_rect){24, 24, 16, 16}, NULL);
        flux_canvas_end(canvas);
        EXPECT(flux_frame_submit(frame) == FLUX_OK);
        EXPECT(flux_frame_present(frame) == FLUX_OK);

        static uint8_t px[BYTES];
        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);
        const uint8_t *py = px + (32 * W + 32) * 4;
        EXPECT(py[0] > 250 && py[1] > 250 && py[2] < 5);
        flux_image_release(yellow);
    }

    /* --- steady state: two identical batched create/settle rounds must
     *     land on the same allocator counts (staging is cached, retire
     *     zombies are swept by the flush's fence wait). --- */
    {
        EXPECT(flux_uploads_begin(d) == FLUX_OK);
        flux_image *a = make_solid(d, 9, 9, 9);
        EXPECT(flux_uploads_flush(d) == FLUX_OK);
        flux_memory_stats s1;
        flux_device_memory_stats(d, &s1);
        flux_image_release(a);

        EXPECT(flux_uploads_begin(d) == FLUX_OK);
        flux_image *b = make_solid(d, 9, 9, 9);
        EXPECT(flux_uploads_flush(d) == FLUX_OK);
        flux_memory_stats s2;
        flux_device_memory_stats(d, &s2);
        EXPECT(s2.live_allocations == s1.live_allocations);
        EXPECT(s2.bytes_in_use == s1.bytes_in_use);
        flux_image_release(b);
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
    {
        struct rusage ru0, ru1, ru2;
        static uint8_t churn[16 * 16 * 4];
        memset(churn, 0x42, sizeof(churn));

        const int warmup = 512;
        for (int i = 0; i < warmup; i++) {
            EXPECT(flux_uploads_begin(d) == FLUX_OK);
            flux_image_desc desc = {
                .type = FLUX_TYPE_IMAGE_DESC,
                .width = 16,
                .height = 16,
                .format = FLUX_FORMAT_RGBA8_UNORM,
                .initial_data = churn,
            };
            flux_image *img = NULL;
            EXPECT(flux_image_create(d, &desc, &img) == FLUX_OK && img);
            EXPECT(flux_uploads_flush(d) == FLUX_OK);
            flux_image_release(img);
        }
        EXPECT(getrusage(RUSAGE_SELF, &ru0) == 0);

        for (int i = 0; i < warmup; i++) {
            EXPECT(flux_uploads_begin(d) == FLUX_OK);
            flux_image_desc desc = {
                .type = FLUX_TYPE_IMAGE_DESC,
                .width = 16,
                .height = 16,
                .format = FLUX_FORMAT_RGBA8_UNORM,
                .initial_data = churn,
            };
            flux_image *img = NULL;
            EXPECT(flux_image_create(d, &desc, &img) == FLUX_OK && img);
            EXPECT(flux_uploads_flush(d) == FLUX_OK);
            flux_image_release(img);
        }
        EXPECT(getrusage(RUSAGE_SELF, &ru1) == 0);
        for (int i = 0; i < warmup; i++) {
            EXPECT(flux_uploads_begin(d) == FLUX_OK);
            flux_image_desc desc = {
                .type = FLUX_TYPE_IMAGE_DESC,
                .width = 16,
                .height = 16,
                .format = FLUX_FORMAT_RGBA8_UNORM,
                .initial_data = churn,
            };
            flux_image *img = NULL;
            EXPECT(flux_image_create(d, &desc, &img) == FLUX_OK && img);
            EXPECT(flux_uploads_flush(d) == FLUX_OK);
            flux_image_release(img);
        }
        EXPECT(getrusage(RUSAGE_SELF, &ru2) == 0);

        long w0 = ru0.ru_maxrss, w1 = ru1.ru_maxrss, w2 = ru2.ru_maxrss;
        /* MaxRSS is monotonic; growth between the settled windows is
         * what detects the unbounded driver-state accumulation. The
         * pre-fix ANV behaviour grew ~72 KiB per round; the allowance
         * is a few pages per round at most. */
        EXPECT(w1 - w0 < warmup * 64);
        EXPECT(w2 - w1 < warmup * 64);
    }

    flux_image_release(red);
    flux_image_release(green);
    flux_image_release(blue);
    flux_canvas_destroy(canvas);
    flux_surface_release(s);
    flux_device_release(d);
    TEST_SUMMARY();
}
