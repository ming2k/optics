/*
 * Runtime-icon rendering through the public API only (ADR-0112).
 *
 * This binary deliberately links *only* the shared library — it must never
 * compile icon_runtime.c / icon_mask.c. A runtime SVG registered
 * here is stored in liboptics_ui's own icon registry, and ui's replay (also inside
 * liboptics_ui) resolves it from that same registry. Building a local copy of the
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
#include "optics_test.h"
#include "ui_test.h"
#include <optics/gfx/canvas.h>
#include <optics/gfx/canvas_cpu.h>
#include <optics/ui.h>
#include <optics/ui/icon.h>

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
static ink_counts render_and_classify(optics_ui_icon_id id, float size) {
    ink_counts k = {0, 0, 0, 0};
    uint32_t box = (uint32_t)size + 8u;
    optics_ui *ui = nullptr;
    if (optics_ui_create(&(optics_ui_desc){.scale = 1.0f}, &ui) != OPTICS_GFX_OK)
        return k;

    optics_ui_input in = {.display_size = {(float)box, (float)box}, .dt_seconds = 0.016f};
    optics_ui_begin(ui, &in);
    optics_ui_icon(ui, &(optics_ui_icon_opts){.id = id, .size = size});
    optics_ui_end(ui);

    optics_gfx_canvas *c = nullptr;
    if (optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = box,
                                                           .height = box},
                                 &c) != OPTICS_GFX_OK) {
        optics_ui_release(ui);
        return k;
    }
    optics_gfx_color clear = {};
    if (optics_gfx_canvas_begin(
            c, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                              .clear_color = &clear}) == OPTICS_GFX_OK) {
        optics_ui_scene_snapshot *s = nullptr;
        if (optics_ui_snapshot_create(ui, &s) == OPTICS_GFX_OK) {
            (void)optics_ui_snapshot_submit(s, c);
            optics_ui_snapshot_release(s);
        }
        (void)optics_gfx_canvas_end(c);
    }

    uint32_t w = 0, h = 0, stride = 0;
    const uint8_t *px = optics_gfx_canvas_cpu_pixels(c, &w, &h, &stride);
    if (px) {
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                const uint8_t *p = &px[(size_t)y * stride + (size_t)x * 4];
                int r = p[0], g = p[1], b = p[2];
                if (r <= 40 && g <= 40 && b <= 40)
                    continue; /* background or dim AA fringe below primary threshold */
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
    optics_gfx_canvas_release(c);
    optics_ui_release(ui);
    return k;
}

int main(void) {
    /* Registration goes through the library that will draw it. */
    optics_ui_icon_id id = optics_ui_icon_register_svg(SVG_MULTI_RUN);
    OT_EXPECT(id != OPTICS_UI_ICON_INVALID);
    OT_EXPECT((int)id >= (int)OPTICS_UI_ICON_COUNT);
    /* The library's own registry must resolve it — this is the exact assertion
     * that fails if a second registry exists anywhere in the binary. */
    OT_EXPECT(optics_ui_icon_info(id, nullptr) != nullptr);

    /* ---- mask path: small size, all per-run colours survive ---- */
    {
        ink_counts k = render_and_classify(id, 20.0f);
        OT_EXPECT(k.red > 0);
        OT_EXPECT(k.green > 0);
        OT_EXPECT(k.blue > 0);
        /* Nothing should have collapsed to the theme colour. */
        OT_EXPECT(k.other < k.red + k.green + k.blue);
    }

    /* ---- vector fallback: same icon past the mask bake limit ---- *
     * Quality drops (that is the documented trade-off) but the icon must still
     * declare all three colours rather than collapsing to one paint. */
    {
        ink_counts k = render_and_classify(id, 300.0f);
        OT_EXPECT(k.red > 100);
        OT_EXPECT(k.green > 100);
        OT_EXPECT(k.blue > 100);
        OT_EXPECT(k.other < 50); /* no wholesale collapse to theme colour */
    }

    /* ---- built-in icon: collapsed single run follows the theme ---- */
    {
        ink_counts k = render_and_classify(OPTICS_UI_ICON_CHECK, 20.0f);
        OT_EXPECT(k.red == 0 && k.green == 0 && k.blue == 0);
        OT_EXPECT(k.other > 0);
    }

    return OT_REPORT();
}
