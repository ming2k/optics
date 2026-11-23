/*
 * Coverage-plane contracts (ADR-0112, ADR-0119).
 *
 * The mask cache in ui depends on two gfx guarantees on the software
 * rasteriser:
 *
 *   1. The 1x analytic coverage plane carries rich continuous coverage (>16
 *      levels), beating any hardware MSAA sample budget with no supersample
 *      multiplier needed.
 *   2. optics_gfx_canvas_cpu_coverage returns a width x height plane that agrees
 *      bit-for-bit with the alpha channel of optics_gfx_canvas_cpu_pixels. Masks are
 *      baked from the former and composited through the latter; if the two
 *      drifted apart, baked coverage would not match live draws.
 */
#include "optics_test.h"
#include <optics/gfx/canvas.h>
#include <optics/gfx/canvas_cpu.h>
#include <optics/gfx/canvas_helpers.h>
#include <optics/gfx/math.h>

#define CANVAS_W 48u
#define CANVAS_H 48u

/* Count distinct partial-alpha values (1..254) — a proxy for how many coverage
 * steps a pixel can express. Higher is smoother. */
static int partial_levels(const uint8_t *buf, uint32_t w, uint32_t h, uint32_t stride,
                          uint32_t channels) {
    int seen[256] = {};
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t v = buf[(size_t)y * stride + (size_t)x * channels + (channels - 1)];
            seen[v] = 1;
        }
    }
    int count = 0;
    for (int i = 1; i < 255; ++i)
        count += seen[i];
    return count;
}

/* Draw one stroked circle path — general path geometry, so it goes through the
 * tessellator and therefore through sample coverage. */
static void draw_stroked_path(optics_gfx_canvas *c) {
    optics_gfx_arena arena;
    if (optics_gfx_arena_init(&arena, 32u * 1024u, nullptr) != OPTICS_GFX_OK)
        return;
    optics_gfx_path *p = nullptr;
    if (optics_gfx_path_create(&p, &arena) == OPTICS_GFX_OK) {
        optics_gfx_path_add_circle(p, CANVAS_W * 0.5f, CANVAS_H * 0.5f, 16.0f);
        optics_gfx_geometry g = {.kind = OPTICS_GFX_GEOM_PATH, .path = {.path = p}};
        g.stroke.width = 1.2f;
        optics_gfx_brush b =
            optics_gfx_brush_solid(optics_gfx_color_rgba_premul(255, 255, 255, 255));
        optics_gfx_canvas_draw_geometry(
            c, &(optics_gfx_canvas_draw_geometry_opts){.geom = &g, .brush = &b});
    }
    optics_gfx_arena_deinit(&arena);
}

static optics_gfx_canvas *bake_canvas(void) {
    optics_gfx_canvas *c = nullptr;
    optics_gfx_canvas_desc d = OPTICS_GFX_CANVAS_DESC_INIT;
    d.backend = OPTICS_GFX_CANVAS_BACKEND_CPU;
    d.width = CANVAS_W;
    d.height = CANVAS_H;
    if (optics_gfx_canvas_create(&d, &c) != OPTICS_GFX_OK)
        return nullptr;
    optics_gfx_color none = {};
    if (optics_gfx_canvas_begin(
            c, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                              .clear_color = &none}) != OPTICS_GFX_OK) {
        optics_gfx_canvas_release(c);
        return nullptr;
    }
    draw_stroked_path(c);
    if (optics_gfx_canvas_end(c) != OPTICS_GFX_OK) {
        optics_gfx_canvas_release(c);
        return nullptr;
    }
    return c;
}

int main(void) {
    /* ---- 1x analytic coverage exceeds legacy supersample/MSAA budgets ---- */
    optics_gfx_canvas *c = bake_canvas();
    OT_EXPECT(c != nullptr);
    if (c) {
        uint32_t w = 0, h = 0, stride = 0;
        const uint8_t *cov = optics_gfx_canvas_cpu_coverage(c, &w, &h, &stride);
        OT_EXPECT(cov != nullptr);
        OT_EXPECT(w == CANVAS_W && h == CANVAS_H);
        OT_EXPECT(stride == CANVAS_W); /* one byte per pixel, tightly packed */
        if (cov) {
            int levels = partial_levels(cov, w, h, stride, 1);
            OT_EXPECT(levels > 16);
        }

        /* ---- the coverage plane agrees with the pixel alpha plane ---- */
        uint32_t cw = 0, ch = 0, cs = 0, pw = 0, ph = 0, ps = 0;
        cov = optics_gfx_canvas_cpu_coverage(c, &cw, &ch, &cs);
        const uint8_t *px = optics_gfx_canvas_cpu_pixels(c, &pw, &ph, &ps);
        OT_EXPECT(cov != nullptr && px != nullptr);
        OT_EXPECT(cw == pw && ch == ph);
        if (cov && px) {
            int mismatches = 0;
            for (uint32_t y = 0; y < ch; ++y) {
                for (uint32_t x = 0; x < cw; ++x) {
                    int a = px[(size_t)y * ps + (size_t)x * 4 + 3];
                    int cv = cov[(size_t)y * cs + x];
                    if (a != cv)
                        mismatches++;
                }
            }
            OT_EXPECT(mismatches == 0);
        }
        OT_EXPECT(optics_gfx_canvas_cpu_coverage(nullptr, &w, &h, &stride) == nullptr);
        optics_gfx_canvas_release(c);
    }

    return OT_REPORT();
}
