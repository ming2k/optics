/*
 * Runtime-icon rendering through the public API only (ADR-0112).
 *
 * This binary deliberately links *only* the shared library — it must never
 * compile icon_runtime.c / icon_mask.c / icon_svg.c. A runtime SVG registered
 * here is stored in liblens's own icon registry, and lens's replay (also inside
 * liblens) resolves it from that same registry. Building a local copy of the
 * registry would give the test two registries: the registration would land in the
 * local one while replay read the library's, and every icon would silently render
 * blank. That failure mode is invisible in a pixel assertion — it looks exactly
 * like "the mask path is broken"—so the link line is part of the test's
 * correctness, not an implementation detail.
 *
 * What it pins, end to end:
 *   - the mask path renders a multi-run (multi-colour) SVG with every declared
 *     paint colour intact, at small sizes;
 *   - the vector fallback — forced here by an icon larger than the mask bake
 *     limit — renders the same icon with the same per-run colours, so quality
 *     degrades but content never does;
 *   - a built-in icon still follows the theme colour (collapsed single run).
 */
#include "test_helpers.h"
#include <flux/canvas.h>
#include <flux/canvas_cpu.h>
#include <lens/icon.h>
#include <lens/lens.h>

/* Three explicit paint runs: red ring, green hub, blue tick. Every run carries a
 * colour that cannot be confused with the others or with the theme. */
static const char SVG_MULTI_RUN[] =
    "<svg viewBox='0 0 24 24'>"
    "<circle cx='12' cy='12' r='9' fill='none' stroke='#ff0000' stroke-width='2'/>"
    "<circle cx='12' cy='12' r='3' fill='#00ff00'/>"
    "<path d='M8 12 L11 15 L16 8' fill='none' stroke='#0000ff' stroke-width='2'/>"
    "</svg>";

typedef struct ink_counts {
    long red, green, blue, other;
} ink_counts;

/* Render `id` at `size` into a CPU canvas and classify the ink by hue. */
static ink_counts render_and_classify(lens_icon_id id, float size) {
    ink_counts k = {0, 0, 0, 0};
    uint32_t box = (uint32_t)size + 8u;
    lens *ui = nullptr;
    if (lens_create(&(lens_desc){.scale = 1.0f}, &ui) != FLUX_OK)
        return k;

    lens_input in = {.display_size = {(float)box, (float)box}, .dt_seconds = 0.016f};
    lens_begin(ui, &in);
    lens_icon(ui, &(lens_icon_opts){.id = id, .size = size});
    lens_end(ui);

    flux_canvas *c = nullptr;
    if (flux_canvas_create_cpu(box, box, 1.0f, &c) != FLUX_OK) {
        lens_release(ui);
        return k;
    }
    flux_color clear = {0};
    if (flux_canvas_begin(c, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                      .clear_color = &clear}) == FLUX_OK) {
        lens_scene_snapshot *s = nullptr;
        if (lens_snapshot_create(ui, &s) == FLUX_OK) {
            (void)lens_snapshot_submit(s, c);
            lens_snapshot_release(s);
        }
        (void)flux_canvas_end(c);
    }

    uint32_t w = 0, h = 0, stride = 0;
    const uint8_t *px = flux_canvas_cpu_pixels(c, &w, &h, &stride);
    if (px) {
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                const uint8_t *p = &px[(size_t)y * stride + (size_t)x * 4];
                int r = p[0], g = p[1], b = p[2];
                if (r + g + b < 20)
                    continue; /* background */
                if (r > g + 40 && r > b + 40)
                    k.red++;
                else if (g > r + 40 && g > b + 40)
                    k.green++;
                else if (b > r + 40 && b > g + 40)
                    k.blue++;
                else
                    k.other++; /* theme-coloured ink: a run lost its colour */
            }
        }
    }
    flux_canvas_release(c);
    lens_release(ui);
    return k;
}

int main(void) {
    /* Registration goes through the library that will draw it. */
    lens_icon_id id = lens_icon_register_svg(SVG_MULTI_RUN);
    CHECK(id != LENS_ICON_INVALID);
    CHECK((int)id >= (int)LENS_ICON_COUNT);
    /* The library's own registry must resolve it — this is the exact assertion
     * that fails if a second registry exists anywhere in the binary. */
    CHECK(lens_icon_info(id, nullptr) != nullptr);

    /* ---- mask path: small size, all per-run colours survive ---- */
    {
        ink_counts k = render_and_classify(id, 20.0f);
        CHECK(k.red > 0);
        CHECK(k.green > 0);
        CHECK(k.blue > 0);
        /* Nothing should have collapsed to the theme colour. */
        CHECK(k.other < k.red + k.green + k.blue);
    }

    /* ---- vector fallback: same icon past the mask bake limit ---- *
     * Quality drops (that is the documented trade-off) but the icon must still
     * declare all three colours rather than collapsing to one paint. */
    {
        ink_counts k = render_and_classify(id, 300.0f);
        CHECK(k.red > 100);
        CHECK(k.green > 100);
        CHECK(k.blue > 100);
        CHECK(k.other < 50); /* no wholesale collapse to theme colour */
    }

    /* ---- built-in icon: collapsed single run follows the theme ---- */
    {
        ink_counts k = render_and_classify(LENS_ICON_CHECK, 20.0f);
        CHECK(k.red == 0 && k.green == 0 && k.blue == 0);
        CHECK(k.other > 0);
    }

    return TEST_REPORT();
}
