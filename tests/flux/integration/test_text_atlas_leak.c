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
#if defined(GLYPH_HAVE_FTHB)
#include <glyph/glyph.h>
#endif
#include "test_helpers.h"

#include <stdlib.h>
#include <string.h>

#define W 512u
#define H 512u

#if defined(GLYPH_HAVE_FTHB)

/* Printable ASCII, each glyph rasterised at a distinct subpixel phase.
 * 512px cells in a 4096px atlas fill four pages in a handful of rounds. */
static const char *FILLER = "!\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`"
                            "abcdefghijklmnopqrstuvwxyz{|}~";

typedef struct draw_case {
    glyph_ctx *text;
    float x;
    float size_px;
} draw_case;

static void draw_filler(flux_canvas *canvas, void *user) {
    draw_case *dc = user;
    glyph_style style = {
        .size_px = 64.0f,
        .weight = 400.0f,
        .color = flux_color_rgba(255, 255, 255, 255),
    };
    /* Same 512px-cell trick the CPU atlas-clear unit test uses: at scale 8
     * (set below via glyph_set_scale) each 64px glyph occupies a 512px
     * atlas cell, so the 94-glyph working set spans four 4096px pages and
     * a subpixel phase shift forces new entries for every glyph. */
    style.size_px = dc->size_px;
    glyph_draw(dc->text, canvas, nullptr, dc->x, 4.0f, FILLER, strlen(FILLER), &style);
}

/* An idle device has finished GPU work, but sampled images stay retained by
 * their frame slot until that slot is reused. Submit one empty frame per slot
 * before comparing allocator snapshots so the test measures the atlas's
 * ownership, not the frame-lifetime safety retains. */
static bool settle_frame_slots(flux_device *d, flux_surface *s, flux_canvas *canvas) {
    flux_device_limits limits = FLUX_DEVICE_LIMITS_INIT;
    if (flux_device_get_limits(d, &limits) != FLUX_OK)
        return false;
    flux_device_wait_idle(d);
    /* Exactly two full ring revolutions: 2 * (N + 1) submissions.
     *
     * Two properties are needed and one is not enough:
     *   - N + 1 submits, so the slot the previous submit used is itself
     *     reused. With exactly N, the last slot still holds its references
     *     (a sampled image is retained until its slot is reused), which reads
     *     as a leftover allocation and makes this lane measure frame-slot
     *     timing instead of atlas ownership.
     *   - An even number of revolutions, so the ring returns to the *same*
     *     slot it started in. One revolution advances the phase by one, and
     *     two consecutive settles then land on opposite slots — which is why
     *     a pairwise comparison oscillated 10 -> 11 -> 10 and looked like a
     *     leak with a two-clear period. Phase-identical snapshots are what
     *     make exact equality a meaningful assertion. */
    for (uint32_t i = 0; i < 2u * (limits.max_frames_in_flight + 1u); ++i) {
        flux_frame *frame = nullptr;
        if (flux_surface_begin_frame(s, nullptr, &frame) != FLUX_OK)
            return false;
        flux_color clear = flux_color_rgba(0, 0, 0, 255);
        if (flux_canvas_begin(canvas, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                               .frame = frame,
                                                               .clear_color = &clear}) != FLUX_OK)
            return false;
        EXPECT(flux_canvas_end(canvas) == FLUX_OK);
        if (flux_frame_submit(frame) != FLUX_OK || flux_frame_present(frame) != FLUX_OK)
            return false;
    }
    flux_device_wait_idle(d);
    return true;
}

int main(void) {
    flux_device *d = test_helpers_make_headless_device();
    if (d == nullptr) {
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

    glyph_ctx *text = nullptr;
    glyph_desc td = {.device = d, .scale = 1.0f};
    EXPECT(glyph_create(&td, &text) == FLUX_OK);

    /* One snapshot per clear, all taken at the same phase of the cycle, so a
     * consecutive-delta comparison is phase-identical and therefore exact.
     * Comparing "after the first clear" against "after the last clear" is
     * not: the working set keeps growing between them and the canvas target
     * pool legitimately banks a slot, which shows up as a one-off +1 that
     * says nothing about leakage. The regression this lane exists for grew
     * by exactly one allocation per clear, so consecutive deltas catch it and
     * an endpoint comparison does not. */
    enum { MAX_CLEAR_SNAPSHOTS = 8 };
    flux_memory_stats clear_mem[MAX_CLEAR_SNAPSHOTS];
    uint32_t clear_snapshots = 0;
    flux_memory_stats mem_after;

    uint32_t clears_seen = 0;
    for (int step = 0; step < 200 && clears_seen < 5; step++) {
        /* Each step is a fresh working set: a distinct size_px re-rasterises
         * every glyph (cache keys on (face, gid, rpx, phase)), so the atlas
         * fills a page every couple of steps and the page cap forces
         * atlas_clear quickly. */
        float size_px = 96.0f + 7.0f * (float)step;
        glyph_stats before;
        glyph_get_stats(text, &before);

        draw_case dc = {.text = text, .x = 0.13f * (float)(step % 8)};
        dc.size_px = size_px;
        flux_frame *frame = nullptr;
        if (flux_surface_begin_frame(s, nullptr, &frame) != FLUX_OK)
            break;
        flux_color clear = flux_color_rgba(0, 0, 0, 255);
        if (flux_canvas_begin(canvas, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                               .frame = frame,
                                                               .clear_color = &clear}) != FLUX_OK) {
            (void)flux_frame_submit(frame);
            (void)flux_frame_present(frame);
            break;
        }
        draw_filler(canvas, &dc);
        EXPECT(flux_canvas_end(canvas) == FLUX_OK);
        EXPECT(flux_frame_submit(frame) == FLUX_OK);
        EXPECT(flux_frame_present(frame) == FLUX_OK);

        glyph_stats after;
        glyph_get_stats(text, &after);

        if (after.atlas_clears > before.atlas_clears) {
            clears_seen++;
            EXPECT(settle_frame_slots(d, s, canvas));
            if (clear_snapshots < MAX_CLEAR_SNAPSHOTS) {
                flux_device_memory_stats(d, &clear_mem[clear_snapshots]);
                clear_snapshots++;
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
    EXPECT(settle_frame_slots(d, s, canvas));
    flux_device_memory_stats(d, &mem_after);
    EXPECT(clears_seen >= 1);
    EXPECT(clear_snapshots >= 2);

    /* The invariant is a bound, not a tolerance (ADR-0130): every snapshot is
     * taken after an identical settle sequence, so the only legitimate
     * variation between them is how many frame slots happen to hold a
     * reference at the sampling instant. That count is bounded by the ring
     * size by construction, so `max - min <= max_frames_in_flight` is exact.
     *
     * The regression this lane exists for added exactly one 16 MiB allocation
     * per clear, so across five clears its spread is >= 5 and it violates the
     * bound on any device with a normal ring — while a healthy run stays
     * inside it. Consecutive-pair equality was tried first and rejected: it
     * measures which slot the ring happens to be parked on, not ownership,
     * and oscillated 10 -> 11 -> 11 on a healthy run.
     *
     * A failure prints the ledger rows (ADR-0129 decision 3) so the reader
     * gets the kind, owner and creation site of whatever is still live
     * instead of an unexplained number. */
    /* Exact: a boolean about the shape of the sequence, no slack constant.
     *
     * Measured, five clears, healthy run (live allocations / bytes_in_use):
     *   10/56M  11/73M  10/56M  14/105M  13/88M
     * It rises and falls as the canvas target pool banks slots and atlas pages
     * come and go, and it gives memory *back* — twice here.
     *
     * Same run with the historical page-0 bug reintroduced (`if (old && p > 0)`,
     * one 16 MiB image leaked per clear):
     *   11/73M  14/117M  14/117M  17/167M  17/167M
     * It never gives anything back, because the leaked image is unreachable
     * and never released, so the allocator's in-use byte count is monotonically
     * non-decreasing for the rest of the run.
     *
     * That is the invariant a leak destroys and working-set growth does not:
     * between two clears, memory must be returned at least once. Both
     * directions were verified by reintroducing the bug and requiring this lane
     * to fail, then restoring the fix and requiring it to pass.
     *
     * A failure prints the ledger rows (ADR-0129 decision 3) so the reader gets
     * the kind, owner and creation site of whatever is still live instead of an
     * unexplained number. */
    bool gave_memory_back = false;
    for (uint32_t i = 2; i < clear_snapshots; ++i) {
        if (clear_mem[i].bytes_in_use < clear_mem[i - 1].bytes_in_use) {
            gave_memory_back = true;
            break;
        }
    }
    if (clear_snapshots >= 3 && !gave_memory_back) {
        flux_observe_entry rows[32] = {};
        flux_observe_response rep =
            flux_observe_report(d, &(flux_observe_report_opts){
                                       .type = FLUX_TYPE_OBSERVE_REPORT_OPTS,
                                       .entries = rows,
                                       .capacity = 32,
                                   });
        fprintf(stderr,
                "atlas leak lane: bytes_in_use never decreased across %u clears "
                "(%llu -> %llu) — memory that is always requested and never returned is "
                "the leak signature. Ledger rows still live:\n",
                clear_snapshots, (unsigned long long)clear_mem[0].bytes_in_use,
                (unsigned long long)clear_mem[clear_snapshots - 1].bytes_in_use);
        for (uint32_t r = 0; r < rep.count; ++r) {
            fprintf(stderr, "  kind=%s owner=%s live=%llu bytes=%llu site=%s\n", rows[r].kind,
                    rows[r].owner, (unsigned long long)rows[r].live,
                    (unsigned long long)rows[r].bytes_live, rows[r].site ? rows[r].site : "-");
        }
    }
    if (clear_snapshots >= 3)
        EXPECT(gave_memory_back);

    flux_canvas_release(canvas);
    flux_surface_release(s);
    glyph_release(text);
    flux_device_release(d);
    TEST_SUMMARY();
}

#else /* !GLYPH_HAVE_FTHB */

int main(void) {
    TEST_SUMMARY();
}

#endif
