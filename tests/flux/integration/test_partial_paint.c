/*
 * Partial (damage-rect) passes on a swapchain — ADR-0144.
 *
 * A damage-rect pass rasterises a sub-rectangle and, with a LOAD, leaves the
 * rest of the destination alone. That is sound for a caller-owned target image
 * (one persistent buffer: successive partial passes accumulate) and unsound for
 * a swapchain, which is a *rotation* of several images. A partial pass advances
 * only the acquired image, so the images accumulate different patch sets and
 * the compositor — cycling through them — shows a different UI state every
 * frame.
 *
 * The rule asserted here: a sub-rect pass on a surface paints the full extent,
 * so every image in the rotation is identical by construction.
 *
 * The failure this pins was measured on the material gallery before the rule:
 * one damage rect reached the five images 31, 34, 35, 18 and 18 times. A test
 * that merely checks "the pixels look defined" cannot see that, so this asserts
 * the mechanical consequence instead — after a sub-rect pass whose clear colour
 * is uniform, pixels OUTSIDE the requested rectangle must also carry that clear
 * colour. If the pass honoured the sub-rectangle, they would still hold
 * whatever the image held before.
 */
#include "test_helpers.h"
#include <flux/flux.h>
#include <flux/vulkan.h>

#include <string.h>

#define W 64u
#define H 64u
#define BYTES (W * H * 4u)
/* Requested sub-rectangle: the left half. */
#define SUB_W (W / 2u)

static const uint8_t *px_at(const uint8_t *px, uint32_t x, uint32_t y) {
    return px + (y * W + x) * 4u;
}

static bool near_ch(uint8_t got, int want, int tol) {
    int d = (int)got - want;
    return d >= -tol && d <= tol;
}

/* Green, fully opaque, in the 8-bit readback. Tolerances absorb output dither. */
static bool is_clear_green(const uint8_t *p) {
    return near_ch(p[0], 0, 3) && near_ch(p[1], 255, 3) && near_ch(p[2], 0, 3) && p[3] > 250;
}

int main(void) {
    flux_device *d = test_helpers_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_partial_paint: no Vulkan device; skipping\n");
        TEST_SUMMARY();
        return 0;
    }

    flux_surface *s = nullptr;
    {
        flux_surface_desc sd = FLUX_SURFACE_DESC_INIT;
        sd.width = W;
        sd.height = H;
        EXPECT(flux_surface_create(d, &sd, &s) == FLUX_OK);
        EXPECT(s != nullptr);
    }

    flux_canvas *canvas = nullptr;
    {
        flux_canvas_desc cd = FLUX_CANVAS_DESC_INIT;
        cd.surface = s;
        EXPECT(flux_canvas_create(&cd, &canvas) == FLUX_OK);
    }

    static uint8_t px[BYTES];
    const flux_color green = flux_color_rgba(0, 255, 0, 255);

    flux_surface_info info;
    flux_surface_get_info(s, &info);

    /* Every slot in the rotation, so a per-image discrepancy cannot hide behind
     * the first acquire. */
    for (uint32_t i = 0; i < info.image_count + 1; ++i) {
        flux_frame *f = nullptr;
        EXPECT(flux_surface_begin_frame(s, nullptr, &f) == FLUX_OK);
        EXPECT(flux_canvas_begin(canvas, &(flux_canvas_pass_desc){
                                             .type = FLUX_TYPE_CANVAS_PASS_DESC,
                                             .frame = f,
                                             .clear_color = &green,
                                             .render_offset_x = 0,
                                             .render_offset_y = 0,
                                             .render_width = SUB_W,
                                             .render_height = H,
                                         }) == FLUX_OK);
        EXPECT(flux_canvas_end(canvas) == FLUX_OK);
        EXPECT(flux_frame_submit(f) == FLUX_OK);
        EXPECT(flux_frame_present(f) == FLUX_OK);

        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);

        /* Inside the requested rectangle: painted. */
        EXPECT(is_clear_green(px_at(px, SUB_W / 4, H / 2)));
        /* Outside it: painted too, because the pass was widened. This is the
         * assertion that fails if a surface pass honours the sub-rectangle. */
        EXPECT(is_clear_green(px_at(px, W - 4, H / 2)));
        EXPECT(is_clear_green(px_at(px, W - 1, 0)));
        EXPECT(is_clear_green(px_at(px, SUB_W + 1, H - 1)));
        /* Corners, where a stale image would show through first. */
        EXPECT(is_clear_green(px_at(px, W - 1, H - 1)));
    }

    flux_canvas_release(canvas);
    flux_surface_release(s);
    flux_device_release(d);
    TEST_SUMMARY();
    return 0;
}
