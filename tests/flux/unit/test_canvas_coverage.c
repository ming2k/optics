/*
 * Supersampling and coverage-plane contracts (ADR-0112).
 *
 * The mask cache in lens depends on two flux guarantees that nothing else
 * exercises, so they are pinned here against the software rasteriser:
 *
 *   1. flux_canvas_supersample_desc actually changes the number of coverage
 *      levels a resolved pixel can carry. This is the whole reason the mask
 *      design can beat the display's 4x MSAA budget — if the factor silently
 *      stopped propagating, masks would quietly regress to 5-level banding with
 *      no test failure anywhere.
 *   2. flux_canvas_cpu_coverage returns a width x height plane that agrees with
 *      the alpha channel of flux_canvas_cpu_pixels. Masks are baked from the
 *      former and composited through the latter; if the two resolves drifted
 *      apart, baked coverage would not match what a live draw produces.
 *
 * Coverage is taken from a stroked *path*, deliberately: rounded rects and
 * circles take the analytic SDF pipeline, whose coverage does not depend on the
 * supersample factor at all. A test using them would pass for the wrong reason.
 */
#include "../test_helpers.h"
#include <flux/canvas.h>
#include <flux/canvas_cpu.h>
#include <flux/canvas_helpers.h>
#include <flux/math.h>

#define CANVAS_W 48u
#define CANVAS_H 48u

/* Count distinct partial-alpha values (1..254) — a proxy for how many coverage
 * steps a pixel can express. Higher is smoother. */
static int partial_levels(const uint8_t *buf, uint32_t w, uint32_t h, uint32_t stride,
                          uint32_t channels) {
    int seen[256] = {0};
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
static void draw_stroked_path(flux_canvas *c) {
    flux_arena arena;
    if (flux_arena_init(&arena, 32u * 1024u, nullptr) != FLUX_OK)
        return;
    flux_path *p = nullptr;
    if (flux_path_create(&p, &arena) == FLUX_OK) {
        flux_path_add_circle(p, CANVAS_W * 0.5f, CANVAS_H * 0.5f, 16.0f);
        flux_geometry g = {.kind = FLUX_GEOM_PATH, .path = {.path = p}};
        g.stroke.width = 1.2f;
        flux_brush b = flux_brush_solid(flux_color_rgba_premul(255, 255, 255, 255));
        flux_canvas_draw_geometry(c, &g, &b);
    }
    flux_arena_deinit(&arena);
}

static flux_canvas *bake_canvas(uint32_t supersample) {
    flux_canvas *c = nullptr;
    flux_canvas_desc d = FLUX_CANVAS_DESC_INIT;
    d.backend = FLUX_CANVAS_BACKEND_CPU;
    d.width = CANVAS_W;
    d.height = CANVAS_H;
    flux_canvas_supersample_desc ss = FLUX_CANVAS_SUPERSAMPLE_DESC_INIT;
    ss.supersample = supersample;
    d.next = &ss;
    if (flux_canvas_create(&d, &c) != FLUX_OK)
        return nullptr;
    flux_color none = {0};
    if (flux_canvas_begin(c, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                      .clear_color = &none}) != FLUX_OK) {
        flux_canvas_release(c);
        return nullptr;
    }
    draw_stroked_path(c);
    if (flux_canvas_end(c) != FLUX_OK) {
        flux_canvas_release(c);
        return nullptr;
    }
    return c;
}

int main(void) {
    /* ---- the supersample factor changes expressible coverage ---- */
    int levels[3] = {0, 0, 0};
    const uint32_t factors[3] = {1u, 4u, 8u};
    for (int i = 0; i < 3; ++i) {
        flux_canvas *c = bake_canvas(factors[i]);
        EXPECT(c != nullptr);
        if (!c)
            continue;
        uint32_t w = 0, h = 0, stride = 0;
        const uint8_t *cov = flux_canvas_cpu_coverage(c, &w, &h, &stride);
        EXPECT(cov != nullptr);
        EXPECT(w == CANVAS_W && h == CANVAS_H);
        EXPECT(stride == CANVAS_W); /* one byte per pixel, tightly packed */
        if (cov)
            levels[i] = partial_levels(cov, w, h, stride, 1);
        flux_canvas_release(c);
    }

    /* 1 sample/px is binary coverage; more samples must strictly buy levels. */
    EXPECT(levels[0] == 0);
    EXPECT(levels[1] > levels[0]);
    EXPECT(levels[2] > levels[1]);
    /* The mask design needs far more than the 4x MSAA budget (which caps at 5
     * levels); 8x8 must comfortably exceed it. */
    EXPECT(levels[2] > 8);

    /* ---- out-of-range factors are rejected, never silently clamped ---- */
    {
        flux_canvas *c = nullptr;
        flux_canvas_desc d = FLUX_CANVAS_DESC_INIT;
        d.backend = FLUX_CANVAS_BACKEND_CPU;
        d.width = 16;
        d.height = 16;
        flux_canvas_supersample_desc ss = FLUX_CANVAS_SUPERSAMPLE_DESC_INIT;
        ss.supersample = FLUX_CANVAS_SUPERSAMPLE_MAX + 1u;
        d.next = &ss;
        EXPECT(flux_canvas_create(&d, &c) == FLUX_ERROR_OUT_OF_RANGE);
        EXPECT(c == nullptr);
        EXPECT(FLUX_CANVAS_SUPERSAMPLE_MAX == 8u);

        /* Direct constructor rejects out-of-range the same way. */
        EXPECT(flux_canvas_create_cpu_ss(16, 16, 1.0f, FLUX_CANVAS_SUPERSAMPLE_MAX + 1u, &c) ==
               FLUX_ERROR_OUT_OF_RANGE);
        EXPECT(c == nullptr);

        /* Direct constructor succeeds with in-range factors. */
        EXPECT(flux_canvas_create_cpu_ss(16, 16, 1.0f, 4u, &c) == FLUX_OK);
        EXPECT(c != nullptr);
        flux_canvas_release(c);
    }

    /* ---- the coverage plane agrees with the pixel alpha plane ---- */
    {
        flux_canvas *c = bake_canvas(4u);
        EXPECT(c != nullptr);
        if (c) {
            uint32_t cw = 0, ch = 0, cs = 0, pw = 0, ph = 0, ps = 0;
            const uint8_t *cov = flux_canvas_cpu_coverage(c, &cw, &ch, &cs);
            const uint8_t *px = flux_canvas_cpu_pixels(c, &pw, &ph, &ps);
            EXPECT(cov != nullptr && px != nullptr);
            EXPECT(cw == pw && ch == ph);
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
                /* Both are the same box filter over the same sample buffer, so
                 * they must agree bit-for-bit; allow a one-step rounding slack
                 * only to keep this from being brittle against future resolve
                 * refactors. */
                EXPECT(mismatches == 0);
            }
            flux_canvas_release(c);
        }
    }

    /* ---- a GPU backend has no coverage plane: the slot is optional ---- */
    {
        flux_canvas *c = bake_canvas(0u); /* default factor still resolves */
        EXPECT(c != nullptr);
        if (c) {
            uint32_t w = 0, h = 0, s = 0;
            EXPECT(flux_canvas_cpu_coverage(c, &w, &h, &s) != nullptr);
            EXPECT(flux_canvas_cpu_coverage(nullptr, &w, &h, &s) == nullptr);
            flux_canvas_release(c);
        }
    }

    TEST_SUMMARY();
}
