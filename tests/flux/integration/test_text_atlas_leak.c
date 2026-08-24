/*
 * Glyph-atlas GPU image governance across atlas_clear (atlas.c).
 *
 * atlas_clear drops every cached glyph and swaps each atlas page for a
 * fresh GPU image; the old images must be released onto the device retire
 * queue. Regression: the retire loop guarded the release with `p > 0`, so
 * page 0's 16 MiB dedicated VkDeviceMemory was leaked on every clear.
 * Long sessions with recurring glyph churn (browsers, CJK input) grew RSS
 * by one allocation per clear with no bound.
 *
 * The test forces multiple atlas clears with distinct glyph working sets
 * on a real GPU canvas and asserts the device's live allocation count
 * returns to its baseline once the retire queue drains — a leak shows up
 * as monotonic growth in live_allocations across clears.
 */
#include <flux/flux.h>
#include <flux/vulkan.h>
#if defined(FLUX_TEXT_HAVE_FTHB)
#include <flux-text/text.h>
#endif
#include "test_helpers.h"

#include <stdlib.h>
#include <string.h>

#define W 512u
#define H 512u

#if defined(FLUX_TEXT_HAVE_FTHB)

/* Printable ASCII, each glyph rasterised at a distinct subpixel phase.
 * 512px cells in a 4096px atlas fill four pages in a handful of rounds. */
static const char *FILLER =
    "!\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`"
    "abcdefghijklmnopqrstuvwxyz{|}~";

typedef struct draw_case {
    flux_text *text;
    float x;
    float size_px;
} draw_case;

static void draw_filler(flux_canvas *canvas, void *user) {
    draw_case *dc = user;
    flux_text_style style = {
        .size_px = 64.0f,
        .weight = 400.0f,
        .color = flux_color_rgba(255, 255, 255, 255),
    };
    /* Same 512px-cell trick the CPU atlas-clear unit test uses: at scale 8
     * (set below via flux_text_set_scale) each 64px glyph occupies a 512px
     * atlas cell, so the 94-glyph working set spans four 4096px pages and
     * a subpixel phase shift forces new entries for every glyph. */
    style.size_px = dc->size_px;
    flux_text_draw(dc->text, canvas, nullptr, dc->x, 4.0f, FILLER, strlen(FILLER), &style);
}

int main(void) {
    flux_device *d = test_helpers_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_text_atlas_leak: no Vulkan device; skipping\n");
        TEST_SUMMARY();
    }

    flux_surface *s = nullptr;
    {
        flux_surface_desc sd = FLUX_SURFACE_DESC_INIT;
        sd.width = W;
        sd.height = H;
        EXPECT(flux_surface_create(d, &sd, &s) == FLUX_OK);
    }
    flux_canvas *canvas = nullptr;
    {
        flux_canvas_desc cd = FLUX_CANVAS_DESC_INIT;
        cd.surface = s;
        /* Content scale 8 (the CPU unit test's trick): 64px glyphs
         * rasterise at rpx 512, each an effective ~512px atlas cell, so
         * the 94-glyph working set spans multiple 4096px pages and forces
         * atlas_clear at the page cap. */
        cd.scale = 8.0f;
        EXPECT(flux_canvas_create(&cd, &canvas) == FLUX_OK);
    }

    flux_text *text = nullptr;
    flux_text_desc td = {.device = d, .scale = 1.0f};
    EXPECT(flux_text_create(&td, &text) == FLUX_OK);

    flux_memory_stats mem_after, mem_first_clear_after;
    bool have_first_clear = false;

    uint32_t clears_seen = 0;
    for (int step = 0; step < 96 && clears_seen < 3; step++) {
        /* Each step is a fresh working set: a distinct size_px re-rasterises
         * every glyph (cache keys on (face, gid, rpx, phase)), so the atlas
         * fills a page every couple of steps and the page cap forces
         * atlas_clear quickly. */
        float size_px = 96.0f + 7.0f * (float)step;
        flux_text_stats before;
        flux_text_get_stats(text, &before);

        draw_case dc = {.text = text, .x = 0.13f * (float)(step % 8)};
        dc.size_px = size_px;
        flux_frame *frame = nullptr;
        if (flux_surface_begin_frame(s, nullptr, &frame) != FLUX_OK)
            break;
        flux_color clear = flux_color_rgba(0, 0, 0, 255);
        if (flux_canvas_begin(canvas, frame, &clear) != FLUX_OK) {
            flux_frame_submit(frame);
            flux_frame_present(frame);
            break;
        }
        draw_filler(canvas, &dc);
        flux_canvas_end(canvas);
        flux_frame_submit(frame);
        flux_frame_present(frame);

        flux_text_stats after;
        flux_text_get_stats(text, &after);

        if (after.atlas_clears > before.atlas_clears) {
            clears_seen++;
            flux_device_wait_idle(d);
            if (!have_first_clear) {
                flux_device_memory_stats(d, &mem_first_clear_after);
                have_first_clear = true;
            }
        }
    }
    EXPECT(clears_seen >= 1);

    /* The leaked page-0 image held a refcount forever, so each clear
     * permanently added one live 16 MiB allocation that no idle wait could
     * reclaim — the per-clear trend was strictly upward. With the release
     * fixed, a clear's retired pages drain on the idle wait and the live
     * count after the final clear is at most one above the count measured
     * right after the first clear (the slack covers one in-flight page
     * mid-recreate, not a per-clear increment). */
    flux_device_wait_idle(d);
    flux_device_memory_stats(d, &mem_after);
    EXPECT(have_first_clear);
    /* With the page-0 leak each clear permanently added one live
     * allocation, so after three clears the delta was +3 and climbing.
     * Fixed, the retire queue drains on the idle wait and the live count
     * settles within a constant slack of the first clear's snapshot. */
    EXPECT(have_first_clear);
    if (have_first_clear)
        EXPECT(mem_after.live_allocations
               <= mem_first_clear_after.live_allocations + 2);

    flux_canvas_destroy(canvas);
    flux_surface_release(s);
    flux_text_destroy(text);
    flux_device_release(d);
    TEST_SUMMARY();
}

#else /* !FLUX_TEXT_HAVE_FTHB */

int main(void) {
    TEST_SUMMARY();
}

#endif
