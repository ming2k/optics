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
#include "gfx_test.h"
#include "optics_test.h"
#include <optics/gfx.h>
#include <optics/gfx/vulkan.h>

#define W 64u
#define H 64u

static void expect_scope(const optics_gfx_frame *f, const optics_gfx_rect *rects, uint32_t count,
                         bool full) {
    optics_gfx_rect got[OPTICS_GFX_DAMAGE_CAPACITY];
    uint32_t got_count = 0;
    bool got_full = !full;
    OT_EXPECT(optics_gfx_frame_repaint_scope(f, &(optics_gfx_frame_repaint_scope_opts){
                                                    .out_rects = got,
                                                    .capacity = OPTICS_GFX_DAMAGE_CAPACITY,
                                                    .out_count = &got_count,
                                                    .out_full = &got_full}) == OPTICS_GFX_OK);
    if (got_full != full || (!full && got_count != count)) {
        fprintf(stderr, "scope got full=%d count=%u; want full=%d count=%u\n", got_full, got_count,
                full, count);
        for (uint32_t d = 0; d < got_count; d++)
            fprintf(stderr, "  got %.1f,%.1f %.1fx%.1f\n", got[d].x, got[d].y, got[d].w, got[d].h);
    }
    OT_EXPECT(got_full == full);
    if (full)
        return;
    OT_EXPECT(got_count == count);
    for (uint32_t i = 0; i < count && i < got_count; i++) {
        OT_EXPECT(got[i].x == rects[i].x);
        OT_EXPECT(got[i].y == rects[i].y);
        OT_EXPECT(got[i].w == rects[i].w);
        OT_EXPECT(got[i].h == rects[i].h);
    }
}

static void expect_scanout(const optics_gfx_surface *s, const optics_gfx_rect *rects,
                           uint32_t count, bool full) {
    optics_gfx_rect got[OPTICS_GFX_DAMAGE_CAPACITY];
    uint32_t got_count = 0;
    bool got_full = !full;
    OT_EXPECT(optics_gfx_surface_scanout_damage(s, &(optics_gfx_surface_scanout_damage_opts){
                                                       .out_rects = got,
                                                       .capacity = OPTICS_GFX_DAMAGE_CAPACITY,
                                                       .out_count = &got_count,
                                                       .out_full = &got_full}) == OPTICS_GFX_OK);
    OT_EXPECT(got_full == full);
    if (full)
        return;
    OT_EXPECT(got_count == count);
    for (uint32_t i = 0; i < count && i < got_count; i++) {
        OT_EXPECT(got[i].x == rects[i].x);
        OT_EXPECT(got[i].y == rects[i].y);
        OT_EXPECT(got[i].w == rects[i].w);
        OT_EXPECT(got[i].h == rects[i].h);
    }
}

/* Clear one frame's damage scope. A clear pass records exactly the rects it
 * writes; the delta committed to history is the caller-supplied current damage. */
static optics_gfx_result submit_clear(optics_gfx_surface *s, optics_gfx_canvas *canvas,
                                      const optics_gfx_rect *rects, uint32_t count, bool full,
                                      bool present, const optics_gfx_rect *scan_rects,
                                      uint32_t scan_count, bool scan_full) {
    optics_gfx_frame *frame = nullptr;
    optics_gfx_frame_begin_desc begin = OPTICS_GFX_FRAME_BEGIN_DESC_INIT;
    begin.damage = (optics_gfx_damage){.rects = rects, .count = count};
    begin.damage_full = full;
    optics_gfx_result r = optics_gfx_surface_begin_frame(s, &begin, &frame);
    if (r != OPTICS_GFX_OK)
        return r;

    optics_gfx_color clear = optics_gfx_color_rgba(10, 20, 30, 255);
    r = optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                            .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                            .frame = frame,
                                            .clear_color = &clear,
                                            .damage = {.rects = rects, .count = count},
                                        });
    if (r != OPTICS_GFX_OK)
        return r;
    OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
    r = optics_gfx_frame_submit(frame);
    if (r != OPTICS_GFX_OK)
        return r;
    expect_scanout(s, scan_rects, scan_count, scan_full);
    if (present)
        return optics_gfx_frame_present(frame);
    optics_gfx_frame_abandon(frame);
    return OPTICS_GFX_OK;
}

int main(void) {
    optics_gfx_device *d = optics_gfx_test_make_headless_device_in_flight(3);
    if (!d) {
        fprintf(stderr, "test_frame_damage: no Vulkan device; skipping\n");
        OT_SKIP("no Vulkan device");
    }

    optics_gfx_surface *s = nullptr;
    {
        optics_gfx_surface_desc sd = OPTICS_GFX_SURFACE_DESC_INIT;
        sd.width = W;
        sd.height = H;
        OT_EXPECT(optics_gfx_surface_create(d, &sd, &s) == OPTICS_GFX_OK);
    }
    optics_gfx_canvas *canvas = nullptr;
    {
        optics_gfx_canvas_desc cd = OPTICS_GFX_CANVAS_DESC_INIT;
        cd.surface = s;
        OT_EXPECT(optics_gfx_canvas_create(&cd, &canvas) == OPTICS_GFX_OK);
    }

    const optics_gfx_rect a = {4.0f, 4.0f, 8.0f, 8.0f};
    const optics_gfx_rect b = {24.0f, 6.0f, 6.0f, 9.0f};
    const optics_gfx_rect c = {40.0f, 20.0f, 7.0f, 5.0f};
    const optics_gfx_rect dmg_d = {12.0f, 30.0f, 10.0f, 4.0f};
    const optics_gfx_rect e = {2.0f, 44.0f, 5.0f, 5.0f};
    const optics_gfx_rect f = {16.0f, 50.0f, 9.0f, 3.0f};

    /* Frame 1: image 0 has no content yet; full. The first frame's delta is
     * also full, so later images see the initial desktop change. */
    {
        optics_gfx_frame *frame = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);
        expect_scope(frame, nullptr, 0, true);
        optics_gfx_color clear = optics_gfx_color_rgba(10, 20, 30, 255);
        OT_EXPECT(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                      .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                      .frame = frame,
                                                      .clear_color = &clear,
                                                  }) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        expect_scanout(s, nullptr, 0, true);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
        expect_scanout(s, nullptr, 0, false);
    }

    /* Image 1 first use: repaint scope full, but the committed delta is A. */
    {
        optics_gfx_frame *frame = nullptr;
        optics_gfx_frame_begin_desc begin = OPTICS_GFX_FRAME_BEGIN_DESC_INIT;
        begin.damage = (optics_gfx_damage){.rects = &a, .count = 1};
        begin.damage_full = false;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, &begin, &frame) == OPTICS_GFX_OK);
        expect_scope(frame, nullptr, 0, true);
        optics_gfx_color clear = optics_gfx_color_rgba(10, 20, 30, 255);
        OT_EXPECT(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                      .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                      .frame = frame,
                                                      .clear_color = &clear,
                                                      .damage = {.rects = &a, .count = 1},
                                                  }) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        expect_scanout(s, &a, 1, false);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
    }

    /* Image 2 first use: same, delta B. */
    OT_EXPECT(submit_clear(s, canvas, &b, 1, false, true, &b, 1, false) == OPTICS_GFX_OK);

    /* Image 0 returns after missing A and B: repaint A∪B∪C, scanout C. */
    {
        optics_gfx_frame *frame = nullptr;
        optics_gfx_frame_begin_desc begin = OPTICS_GFX_FRAME_BEGIN_DESC_INIT;
        begin.damage = (optics_gfx_damage){.rects = &c, .count = 1};
        begin.damage_full = false;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, &begin, &frame) == OPTICS_GFX_OK);
        const optics_gfx_rect expected[3] = {a, b, c};
        expect_scope(frame, expected, 3, false);
        optics_gfx_color clear = optics_gfx_color_rgba(10, 20, 30, 255);
        OT_EXPECT(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                      .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                      .frame = frame,
                                                      .clear_color = &clear,
                                                      .damage = {.rects = &c, .count = 1},
                                                  }) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        expect_scanout(s, &c, 1, false);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
    }

    /* Image 1 returns after missing B and C: repaint B∪C∪D. */
    {
        optics_gfx_frame *frame = nullptr;
        optics_gfx_frame_begin_desc begin = OPTICS_GFX_FRAME_BEGIN_DESC_INIT;
        begin.damage = (optics_gfx_damage){.rects = &dmg_d, .count = 1};
        begin.damage_full = false;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, &begin, &frame) == OPTICS_GFX_OK);
        const optics_gfx_rect expected[3] = {b, c, dmg_d};
        expect_scope(frame, expected, 3, false);
        optics_gfx_color clear = optics_gfx_color_rgba(10, 20, 30, 255);
        OT_EXPECT(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                      .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                      .frame = frame,
                                                      .clear_color = &clear,
                                                      .damage = {.rects = &dmg_d, .count = 1},
                                                  }) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        expect_scanout(s, &dmg_d, 1, false);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
    }

    /* Abandoned submission: submission advances image 2's epoch and the
     * scanout ledger, but not the presented epoch. The same image is reused on
     * retry, so its repaint scope is only the new damage F. */
    {
        OT_EXPECT(submit_clear(s, canvas, &e, 1, false, false, &e, 1, false) == OPTICS_GFX_OK);

        optics_gfx_frame *frame = nullptr;
        optics_gfx_frame_begin_desc begin = OPTICS_GFX_FRAME_BEGIN_DESC_INIT;
        begin.damage = (optics_gfx_damage){.rects = &f, .count = 1};
        begin.damage_full = false;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, &begin, &frame) == OPTICS_GFX_OK);
        expect_scope(frame, &f, 1, false);
        optics_gfx_color clear = optics_gfx_color_rgba(10, 20, 30, 255);
        OT_EXPECT(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                      .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                      .frame = frame,
                                                      .clear_color = &clear,
                                                      .damage = {.rects = &f, .count = 1},
                                                  }) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        const optics_gfx_rect expected[2] = {e, f};
        expect_scanout(s, expected, 2, false);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
    }

    optics_gfx_canvas_release(canvas);
    optics_gfx_surface_release(s);
    optics_gfx_device_release(d);
    return OT_REPORT();
}
