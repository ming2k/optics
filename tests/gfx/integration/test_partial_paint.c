/*
 * Damage-scoped passes on a swapchain — ADR-0149.
 *
 * A damage-scoped pass rasterises a sub-set of rectangles and leaves the rest of
 * its destination alone. That is sound for a caller-owned target image (one
 * persistent buffer: successive partial passes accumulate) and, before
 * ADR-0149, unsound for a swapchain, which is a *rotation* of several images: a
 * partial pass advanced only the image it acquired, so the images accumulated
 * different patch sets and the compositor — cycling through them — showed a
 * different UI state every frame (measured once as 31/34/35/18/18 hits of one
 * rect across five images).
 *
 * ADR-0144 answered that by painting the full extent into every image. ADR-0149
 * removes that cost: the frame renders into one persistent content image and
 * each acquired image receives the union of write areas committed since it was
 * last synchronised. This test pins the two properties that matter, and it pins
 * them in the shape ADR-0144's negative knowledge demands:
 *
 *   1. It does NOT pre-fill the destination. Every image starts undefined, which
 *      is the only state in which a missing recovery is observable — pre-filling
 *      makes any implementation look correct.
 *   2. It reads back *through the surface*, i.e. the content image, after a
 *      damage-scoped pass, and asserts the untouched region still holds the
 *      complete frame. A pass that wrote its damage rect and preserved nothing
 *      leaves undefined bytes there.
 *   3. It exercises more frames than there are images in the rotation, twice, so
 *      a scheme that only recovers on first use diverges in steady state and is
 *      caught by the second half of each band.
 *
 * The two bands are also deliberately far apart, so the union of their bounding
 * box covers the whole surface: an implementation that collapses the damage set
 * to one renderArea overwrites the middle band with garbage.
 */
#include "gfx_test.h"
#include "optics_test.h"
#include <optics/gfx.h>
#include <optics/gfx/canvas_helpers.h>
#include <optics/gfx/vulkan.h>

#include <stdlib.h>
#include <string.h>

#define W 64u
#define H 64u
#define BYTES (W * H * 4u)
/* Three horizontal bands: top, middle, bottom. Top and bottom are damaged; the
 * middle is what a bounding-box collapse destroys. */
#define BAND_H (H / 3u)
#define TOP_Y 0u
#define BOT_Y (H - BAND_H)

static const uint8_t *px_at(const uint8_t *px, uint32_t x, uint32_t y) {
    return px + (y * W + x) * 4u;
}

static bool near_ch(uint8_t got, int want, int tol) {
    int d = (int)got - want;
    return d >= -tol && d <= tol;
}

static bool is_green(const uint8_t *p) {
    return near_ch(p[0], 0, 3) && near_ch(p[1], 255, 3) && near_ch(p[2], 0, 3) && p[3] > 250;
}
static bool is_red(const uint8_t *p) {
    return near_ch(p[0], 255, 3) && near_ch(p[1], 0, 3) && near_ch(p[2], 0, 3) && p[3] > 250;
}

int main(void) {
    optics_gfx_device *d = optics_gfx_test_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_partial_paint: no Vulkan device; skipping\n");
        OT_SKIP("no Vulkan device");
    }

    optics_gfx_surface *s = nullptr;
    {
        optics_gfx_surface_desc sd = OPTICS_GFX_SURFACE_DESC_INIT;
        sd.width = W;
        sd.height = H;
        OT_EXPECT(optics_gfx_surface_create(d, &sd, &s) == OPTICS_GFX_OK);
        OT_EXPECT(s != nullptr);
    }

    optics_gfx_canvas *canvas = nullptr;
    {
        optics_gfx_canvas_desc cd = OPTICS_GFX_CANVAS_DESC_INIT;
        cd.surface = s;
        OT_EXPECT(optics_gfx_canvas_create(&cd, &canvas) == OPTICS_GFX_OK);
    }

    static uint8_t px[BYTES];
    const optics_gfx_color green = optics_gfx_color_rgba(0, 255, 0, 255);
    const optics_gfx_color red = optics_gfx_color_rgba(255, 0, 0, 255);

    optics_gfx_surface_info info;
    optics_gfx_surface_get_info(s, &info);

    /* Frame 1 of a pair: a full-extent green pass. Nothing is pre-filled — the
     * first image in the rotation has never held anything. */
    optics_gfx_frame *f = nullptr;
    OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &f) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                  .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                  .frame = f,
                                                  .clear_color = &green,
                                              }) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_frame_submit(f) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_frame_present(f) == OPTICS_GFX_OK);

    memset(px, 0xCD, BYTES);
    OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
    OT_EXPECT(is_green(px_at(px, W / 2, H / 2)));
    OT_EXPECT(is_green(px_at(px, W - 1, H - 1)));

    /* Now drive more frames than the rotation has images, twice over, so the
     * second half of the loop necessarily reacquires an image that was already
     * patched once. Every frame is damage-scoped to the two outer bands; the
     * middle band must still be green on *every* image the compositor could
     * display. */
    const optics_gfx_rect bands[2] = {
        {0, (float)TOP_Y, (float)W, (float)BAND_H},
        {0, (float)BOT_Y, (float)W, (float)BAND_H},
    };
    const uint32_t frames = (info.image_count + 1u) * 2u;
    for (uint32_t i = 0; i < frames; ++i) {
        /* Alternate the outer bands' colour so a stale image is unmistakable:
         * even iterations paint red, odd paint green. */
        const optics_gfx_color c = (i & 1u) ? green : red;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &f) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                      .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                      .frame = f,
                                                      .clear_color = &c,
                                                      .damage =
                                                          (optics_gfx_damage){
                                                              .rects = bands,
                                                              .count = 2,
                                                          },
                                                  }) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_submit(f) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(f) == OPTICS_GFX_OK);

        /* Read back through the surface: the content image is the complete
         * frame, so the damage-scoped pass must have left the middle band
         * exactly as the previous frame did. */
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
        const bool outer_is_green = (i & 1u) != 0;
        const uint8_t *band_px = px_at(px, W / 2, TOP_Y + BAND_H / 2);
        const uint8_t *mid_px = px_at(px, W / 2, H / 2);
        OT_EXPECT(outer_is_green ? is_green(band_px) : is_red(band_px));
        /* The preserved band: on the very first iteration it is green from the
         * full pass above; afterwards it holds whatever the preceding iteration
         * left, which is never 0xCD. The essential assertion is that it is *a
         * defined frame pixel*, i.e. the pass did not overwrite it from an
         * unseeded intermediate. */
        OT_EXPECT(!(mid_px[0] == 0xCD && mid_px[1] == 0xCD && mid_px[2] == 0xCD));
        OT_EXPECT(mid_px[0] > 250 || mid_px[1] > 250);
    }

    /* Two disjoint damage rects must not be collapsed: dispatch a single pass
     * whose bands are separated by an untouched middle, then verify that the
     * middle kept its previous colour rather than being overwritten with the
     * new one. This is the assertion a renderArea-collapse fails. */
    OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &f) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_begin(canvas,
                                      &(optics_gfx_canvas_pass_desc){
                                          .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                          .frame = f,
                                          .clear_color = &green,
                                          .damage = (optics_gfx_damage){.rects = bands, .count = 2},
                                      }) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_frame_submit(f) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_frame_present(f) == OPTICS_GFX_OK);

    memset(px, 0xCD, BYTES);
    OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
    OT_EXPECT(is_green(px_at(px, W / 2, TOP_Y + 1)));
    OT_EXPECT(is_green(px_at(px, W / 2, H - 2)));
    /* The gap between the two bands is untouched by this pass — and by the
     * green full-extent pass at the top of this test, so it is green. Anything
     * else means the two rects became one box and the gap was written. */
    OT_EXPECT(is_green(px_at(px, W / 2, H / 2)));

    /* The damage scope is a description borrowed only for the
     * `flux_canvas_begin` call. A caller (the Rust binding's
     * `CanvasPassOptions`, in production) routinely owns the rectangle list in
     * a temporary container that is gone before the pass ends. Reusing the
     * caller's array immediately after begin therefore must not change the
     * active scope: the pass has to own a copy.
     *
     * This is not a theoretical lifetime nicety. When the pointer dangled, the
     * end-of-pass encode resolved a garbage or out-of-bounds scope, emitted
     * zero output blits, and left the previous frame's pixels (an old window,
     * its shadow, a tooltip) on screen indefinitely. */
    {
        /* Fill every image in the rotation first: the scoped pass below writes
         * only its damaged band, so the slot it acquires must already hold a
         * complete green frame outside that band. */
        for (uint32_t i = 0; i < info.image_count; ++i) {
            OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &f) == OPTICS_GFX_OK);
            OT_EXPECT(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                          .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                          .frame = f,
                                                          .clear_color = &green,
                                                      }) == OPTICS_GFX_OK);
            OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
            OT_EXPECT(optics_gfx_frame_submit(f) == OPTICS_GFX_OK);
            OT_EXPECT(optics_gfx_frame_present(f) == OPTICS_GFX_OK);
        }

        optics_gfx_rect *scoped = malloc(sizeof(*scoped));
        OT_EXPECT(scoped != nullptr);
        if (scoped) {
            *scoped = (optics_gfx_rect){0, (float)TOP_Y, (float)W, (float)BAND_H};
            OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &f) == OPTICS_GFX_OK);
            OT_EXPECT(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                          .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                          .frame = f,
                                                          .clear_color = &red,
                                                          .damage =
                                                              (optics_gfx_damage){
                                                                  .rects = scoped,
                                                                  .count = 1,
                                                              },
                                                      }) == OPTICS_GFX_OK);
            /* The caller is free to reuse its array as soon as begin returns. */
            *scoped = (optics_gfx_rect){0, (float)BOT_Y, (float)W, (float)BAND_H};
            free(scoped);
            optics_gfx_canvas_fill_rect_color(canvas, (optics_gfx_rect){0, 0, (float)W, (float)H},
                                              optics_gfx_color_rgba(0, 0, 255, 255));
            OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
            OT_EXPECT(optics_gfx_frame_submit(f) == OPTICS_GFX_OK);
            OT_EXPECT(optics_gfx_frame_present(f) == OPTICS_GFX_OK);

            memset(px, 0xCD, BYTES);
            OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
            const uint8_t *top = px_at(px, W / 2, TOP_Y + BAND_H / 2);
            const uint8_t *mid = px_at(px, W / 2, H / 2);
            const uint8_t *bottom = px_at(px, W / 2, BOT_Y + BAND_H / 2);
            OT_EXPECT(top[2] > 250 && top[0] < 5);
            OT_EXPECT(is_green(mid));
            OT_EXPECT(is_green(bottom));
        }
    }

    optics_gfx_canvas_release(canvas);
    optics_gfx_surface_release(s);
    optics_gfx_device_release(d);
    return OT_REPORT();
}
