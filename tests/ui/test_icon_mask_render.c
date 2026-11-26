/*
 * Runtime-icon rendering through the public API only (ADR-0112).
 *
 * What it pins, end to end:
 *   - the mask path renders a multi-run (multi-colour) vector icon with every
 *     declared paint colour intact, at small sizes;
 *   - the vector fallback — forced here by an icon larger than the mask bake
 *     limit — renders the same icon with the same per-run colours, so quality
 *     degrades but content never does;
 *   - a built-in chrome icon still follows the theme colour (collapsed single run).
 */
#include "optics_test.h"
#include "ui_test.h"
#include <optics/gfx/canvas.h>
#include <optics/gfx/canvas_cpu.h>
#include <optics/ui.h>
#include <optics/ui/icon.h>

static const optics_ui_icon_cmd MULTI_RUN_CMDS[] = {
    {.kind = OPTICS_UI_ICON_CMD_CIRCLE, .params = {12.0f, 12.0f, 9.0f}},
    {.kind = OPTICS_UI_ICON_CMD_CIRCLE, .params = {12.0f, 12.0f, 3.0f}},
    {.kind = OPTICS_UI_ICON_CMD_MOVE, .params = {8.0f, 12.0f}},
    {.kind = OPTICS_UI_ICON_CMD_LINE, .params = {11.0f, 15.0f}},
    {.kind = OPTICS_UI_ICON_CMD_LINE, .params = {16.0f, 8.0f}},
};

typedef struct ink_counts {
    long red, green, blue, other;
} ink_counts;

/* Render an icon at `size` into a CPU canvas and classify the ink by hue. */
static ink_counts render_and_classify(bool use_multi_run, optics_ui_icon_id builtin_id,
                                      float size) {
    ink_counts k = {0, 0, 0, 0};
    uint32_t box = (uint32_t)size + 8u;
    optics_ui *ui = nullptr;
    if (optics_ui_create(&(optics_ui_desc){.theme = optics_ui_theme_dark(), .scale = 1.0f}, &ui) !=
        OPTICS_GFX_OK)
        return k;

    optics_ui_icon_id id = builtin_id;
    if (use_multi_run) {
        const optics_ui_icon_run runs[3] = {
            {
                .first_cmd = 0u,
                .count = 1u,
                .color = optics_gfx_color_rgba_premul(255, 0, 0, 255),
                .paint = OPTICS_UI_ICON_PAINT_STROKE,
            },
            {
                .first_cmd = 1u,
                .count = 1u,
                .color = optics_gfx_color_rgba_premul(0, 255, 0, 255),
                .paint = OPTICS_UI_ICON_PAINT_FILL,
            },
            {
                .first_cmd = 2u,
                .count = 3u,
                .color = optics_gfx_color_rgba_premul(0, 0, 255, 255),
                .paint = OPTICS_UI_ICON_PAINT_STROKE,
            },
        };
        id = optics_ui_icon_register(ui, &(optics_ui_icon_desc){
                                             .cmds = MULTI_RUN_CMDS,
                                             .count = 5u,
                                             .runs = runs,
                                             .run_count = 3u,
                                         });
        OT_EXPECT(id != OPTICS_UI_ICON_NONE);
        OT_EXPECT(optics_ui_icon_info(ui, id) != nullptr);
    }

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
                    continue;
                if (r > g + 40 && r > b + 40)
                    k.red++;
                else if (g > r + 40 && g > b + 40)
                    k.green++;
                else if (b > r + 40 && b > g + 40)
                    k.blue++;
                else
                    k.other++;
            }
        }
    }
    optics_gfx_canvas_release(c);
    optics_ui_release(ui);
    return k;
}

int main(void) {
    /* ---- mask path: small size, all per-run colours survive ---- */
    {
        ink_counts k = render_and_classify(true, OPTICS_UI_ICON_NONE, 20.0f);
        OT_EXPECT(k.red > 0);
        OT_EXPECT(k.green > 0);
        OT_EXPECT(k.blue > 0);
        OT_EXPECT(k.other < k.red + k.green + k.blue);
    }

    /* ---- vector fallback: same icon past the mask bake limit ---- */
    {
        ink_counts k = render_and_classify(true, OPTICS_UI_ICON_NONE, 300.0f);
        OT_EXPECT(k.red > 100);
        OT_EXPECT(k.green > 100);
        OT_EXPECT(k.blue > 100);
        OT_EXPECT(k.other < 50);
    }

    /* ---- built-in chrome icon: collapsed single run follows the theme ---- */
    {
        ink_counts k = render_and_classify(false, OPTICS_UI_ICON_CHEVRON_RIGHT, 20.0f);
        OT_EXPECT(k.red == 0 && k.green == 0 && k.blue == 0);
        OT_EXPECT(k.other > 0);
    }

    return OT_REPORT();
}
