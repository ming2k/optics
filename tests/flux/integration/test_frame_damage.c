/*
 * Native frame damage ledger (ADR-0149 follow-up).
 *
 * Proves the contract the compositor relies on:
 *   - an image that has never been presented repaints full;
 *   - a presented image receives only the history it missed plus current damage;
 *   - submission commits the image and its epoch even when the external flip is
 *     abandoned, so a Busy retry never forces a full repaint;
 *   - scanout damage names everything the display has not seen, including an
 *     abandoned submission, and is independent of the next image acquired.
 */
#include "test_helpers.h"
#include <flux/flux.h>
#include <flux/vulkan.h>

#define W 64u
#define H 64u

static void expect_scope(const flux_frame *f, const flux_rect *rects, uint32_t count, bool full) {
    flux_rect got[FLUX_DAMAGE_CAPACITY];
    uint32_t got_count = 0;
    bool got_full = !full;
    EXPECT(flux_frame_repaint_scope(f, got, FLUX_DAMAGE_CAPACITY, &got_count, &got_full) ==
           FLUX_OK);
    if (got_full != full || (!full && got_count != count)) {
        fprintf(stderr, "scope got full=%d count=%u; want full=%d count=%u\n", got_full, got_count,
                full, count);
        for (uint32_t d = 0; d < got_count; d++)
            fprintf(stderr, "  got %.1f,%.1f %.1fx%.1f\n", got[d].x, got[d].y, got[d].w, got[d].h);
    }
    EXPECT(got_full == full);
    if (full)
        return;
    EXPECT(got_count == count);
    for (uint32_t i = 0; i < count && i < got_count; i++) {
        EXPECT(got[i].x == rects[i].x);
        EXPECT(got[i].y == rects[i].y);
        EXPECT(got[i].w == rects[i].w);
        EXPECT(got[i].h == rects[i].h);
    }
}

static void expect_scanout(const flux_surface *s, const flux_rect *rects, uint32_t count,
                           bool full) {
    flux_rect got[FLUX_DAMAGE_CAPACITY];
    uint32_t got_count = 0;
    bool got_full = !full;
    EXPECT(flux_surface_scanout_damage(s, got, FLUX_DAMAGE_CAPACITY, &got_count, &got_full) ==
           FLUX_OK);
    EXPECT(got_full == full);
    if (full)
        return;
    EXPECT(got_count == count);
    for (uint32_t i = 0; i < count && i < got_count; i++) {
        EXPECT(got[i].x == rects[i].x);
        EXPECT(got[i].y == rects[i].y);
        EXPECT(got[i].w == rects[i].w);
        EXPECT(got[i].h == rects[i].h);
    }
}

/* Clear one frame's damage scope. A clear pass records exactly the rects it
 * writes; the delta committed to history is the caller-supplied current damage. */
static flux_result submit_clear(flux_surface *s, flux_canvas *canvas, const flux_rect *rects,
                                uint32_t count, bool full, bool present,
                                const flux_rect *scan_rects, uint32_t scan_count, bool scan_full) {
    flux_frame *frame = nullptr;
    flux_frame_begin_desc begin = FLUX_FRAME_BEGIN_DESC_INIT;
    begin.damage = (flux_damage){.rects = rects, .count = count};
    begin.damage_full = full;
    flux_result r = flux_surface_begin_frame(s, &begin, &frame);
    if (r != FLUX_OK)
        return r;

    flux_color clear = flux_color_rgba(10, 20, 30, 255);
    r = flux_canvas_begin(canvas, &(flux_canvas_pass_desc){
                                      .type = FLUX_TYPE_CANVAS_PASS_DESC,
                                      .frame = frame,
                                      .clear_color = &clear,
                                      .damage = {.rects = rects, .count = count},
                                  });
    if (r != FLUX_OK)
        return r;
    EXPECT(flux_canvas_end(canvas) == FLUX_OK);
    r = flux_frame_submit(frame);
    if (r != FLUX_OK)
        return r;
    expect_scanout(s, scan_rects, scan_count, scan_full);
    if (present)
        return flux_frame_present(frame);
    flux_frame_abandon(frame);
    return FLUX_OK;
}

int main(void) {
    flux_device *d = test_helpers_make_headless_device_in_flight(3);
    if (!d) {
        fprintf(stderr, "test_frame_damage: no Vulkan device; skipping\n");
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
        EXPECT(flux_canvas_create(&cd, &canvas) == FLUX_OK);
    }

    const flux_rect a = {4.0f, 4.0f, 8.0f, 8.0f};
    const flux_rect b = {24.0f, 6.0f, 6.0f, 9.0f};
    const flux_rect c = {40.0f, 20.0f, 7.0f, 5.0f};
    const flux_rect dmg_d = {12.0f, 30.0f, 10.0f, 4.0f};
    const flux_rect e = {2.0f, 44.0f, 5.0f, 5.0f};
    const flux_rect f = {16.0f, 50.0f, 9.0f, 3.0f};

    /* Frame 1: image 0 has no content yet; full. The first frame's delta is
     * also full, so later images see the initial desktop change. */
    {
        flux_frame *frame = nullptr;
        EXPECT(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);
        expect_scope(frame, nullptr, 0, true);
        flux_color clear = flux_color_rgba(10, 20, 30, 255);
        EXPECT(flux_canvas_begin(canvas, &(flux_canvas_pass_desc){
                                             .type = FLUX_TYPE_CANVAS_PASS_DESC,
                                             .frame = frame,
                                             .clear_color = &clear,
                                         }) == FLUX_OK);
        EXPECT(flux_canvas_end(canvas) == FLUX_OK);
        EXPECT(flux_frame_submit(frame) == FLUX_OK);
        expect_scanout(s, nullptr, 0, true);
        EXPECT(flux_frame_present(frame) == FLUX_OK);
        expect_scanout(s, nullptr, 0, false);
    }

    /* Image 1 first use: repaint scope full, but the committed delta is A. */
    {
        flux_frame *frame = nullptr;
        flux_frame_begin_desc begin = FLUX_FRAME_BEGIN_DESC_INIT;
        begin.damage = (flux_damage){.rects = &a, .count = 1};
        begin.damage_full = false;
        EXPECT(flux_surface_begin_frame(s, &begin, &frame) == FLUX_OK);
        expect_scope(frame, nullptr, 0, true);
        flux_color clear = flux_color_rgba(10, 20, 30, 255);
        EXPECT(flux_canvas_begin(canvas, &(flux_canvas_pass_desc){
                                             .type = FLUX_TYPE_CANVAS_PASS_DESC,
                                             .frame = frame,
                                             .clear_color = &clear,
                                             .damage = {.rects = &a, .count = 1},
                                         }) == FLUX_OK);
        EXPECT(flux_canvas_end(canvas) == FLUX_OK);
        EXPECT(flux_frame_submit(frame) == FLUX_OK);
        expect_scanout(s, &a, 1, false);
        EXPECT(flux_frame_present(frame) == FLUX_OK);
    }

    /* Image 2 first use: same, delta B. */
    EXPECT(submit_clear(s, canvas, &b, 1, false, true, &b, 1, false) == FLUX_OK);

    /* Image 0 returns after missing A and B: repaint A∪B∪C, scanout C. */
    {
        flux_frame *frame = nullptr;
        flux_frame_begin_desc begin = FLUX_FRAME_BEGIN_DESC_INIT;
        begin.damage = (flux_damage){.rects = &c, .count = 1};
        begin.damage_full = false;
        EXPECT(flux_surface_begin_frame(s, &begin, &frame) == FLUX_OK);
        const flux_rect expected[3] = {a, b, c};
        expect_scope(frame, expected, 3, false);
        flux_color clear = flux_color_rgba(10, 20, 30, 255);
        EXPECT(flux_canvas_begin(canvas, &(flux_canvas_pass_desc){
                                             .type = FLUX_TYPE_CANVAS_PASS_DESC,
                                             .frame = frame,
                                             .clear_color = &clear,
                                             .damage = {.rects = &c, .count = 1},
                                         }) == FLUX_OK);
        EXPECT(flux_canvas_end(canvas) == FLUX_OK);
        EXPECT(flux_frame_submit(frame) == FLUX_OK);
        expect_scanout(s, &c, 1, false);
        EXPECT(flux_frame_present(frame) == FLUX_OK);
    }

    /* Image 1 returns after missing B and C: repaint B∪C∪D. */
    {
        flux_frame *frame = nullptr;
        flux_frame_begin_desc begin = FLUX_FRAME_BEGIN_DESC_INIT;
        begin.damage = (flux_damage){.rects = &dmg_d, .count = 1};
        begin.damage_full = false;
        EXPECT(flux_surface_begin_frame(s, &begin, &frame) == FLUX_OK);
        const flux_rect expected[3] = {b, c, dmg_d};
        expect_scope(frame, expected, 3, false);
        flux_color clear = flux_color_rgba(10, 20, 30, 255);
        EXPECT(flux_canvas_begin(canvas, &(flux_canvas_pass_desc){
                                             .type = FLUX_TYPE_CANVAS_PASS_DESC,
                                             .frame = frame,
                                             .clear_color = &clear,
                                             .damage = {.rects = &dmg_d, .count = 1},
                                         }) == FLUX_OK);
        EXPECT(flux_canvas_end(canvas) == FLUX_OK);
        EXPECT(flux_frame_submit(frame) == FLUX_OK);
        expect_scanout(s, &dmg_d, 1, false);
        EXPECT(flux_frame_present(frame) == FLUX_OK);
    }

    /* Abandoned submission: submission advances image 2's epoch and the
     * scanout ledger, but not the presented epoch. The same image is reused on
     * retry, so its repaint scope is only the new damage F. */
    {
        EXPECT(submit_clear(s, canvas, &e, 1, false, false, &e, 1, false) == FLUX_OK);

        flux_frame *frame = nullptr;
        flux_frame_begin_desc begin = FLUX_FRAME_BEGIN_DESC_INIT;
        begin.damage = (flux_damage){.rects = &f, .count = 1};
        begin.damage_full = false;
        EXPECT(flux_surface_begin_frame(s, &begin, &frame) == FLUX_OK);
        expect_scope(frame, &f, 1, false);
        flux_color clear = flux_color_rgba(10, 20, 30, 255);
        EXPECT(flux_canvas_begin(canvas, &(flux_canvas_pass_desc){
                                             .type = FLUX_TYPE_CANVAS_PASS_DESC,
                                             .frame = frame,
                                             .clear_color = &clear,
                                             .damage = {.rects = &f, .count = 1},
                                         }) == FLUX_OK);
        EXPECT(flux_canvas_end(canvas) == FLUX_OK);
        EXPECT(flux_frame_submit(frame) == FLUX_OK);
        const flux_rect expected[2] = {e, f};
        expect_scanout(s, expected, 2, false);
        EXPECT(flux_frame_present(frame) == FLUX_OK);
    }

    flux_canvas_release(canvas);
    flux_surface_release(s);
    flux_device_release(d);
    TEST_SUMMARY();
}
