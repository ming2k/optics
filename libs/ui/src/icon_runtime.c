/* icon_runtime.c — runtime SVG icon registration (optics_ui_icon_register_svg).
 *
 * Built-in icons ship as generated optics_ui_icon_cmd tables (icon_data.c); this
 * file lets applications register their own SVG assets at runtime and use
 * them through every icon widget exactly like a built-in.
 *
 * Implemented via the svg vector-document compiler (ADR-0150, ADR-0161).
 */

#include "internal.h"
#include <math.h>
#include <optics/svg.h>
#include <optics/ui/icon.h>
#include <stdlib.h>
#include <string.h>

enum {
    OPTICS_UI_ICON_MAX_CMDS = 8192u,         /* per-icon verb cap */
    OPTICS_UI_ICON_MAX_RUNTIME = 4096u,      /* registry slot cap */
    OPTICS_UI_ICON_MAX_SVG_BYTES = 1u << 20, /* input sanity bound */
};

typedef struct optics_ui__runtime_icon {
    optics_ui_icon_desc desc; /* cmds and runs owned by the entry, never freed */
    uint8_t mode;             /* OPTICS_UI_ICON_RENDER_* */
} optics_ui__runtime_icon;

/* Process-global registry: icons outlive any ui context. */
static optics_ui__runtime_icon *g_runtime_icons;
static uint32_t g_runtime_count;
static uint32_t g_runtime_cap;

bool optics_ui__icon_valid(int32_t id) {
    if (id < 0)
        return false;
    if (id < (int32_t)OPTICS_UI_ICON_COUNT)
        return true;
    return (uint32_t)(id - (int32_t)OPTICS_UI_ICON_COUNT) < g_runtime_count;
}

const optics_ui_icon_desc *optics_ui__icon_desc(int32_t id) {
    if (id < 0)
        return nullptr;
    if (id < (int32_t)OPTICS_UI_ICON_COUNT)
        return &optics_ui_icon_table[id];
    uint32_t index = (uint32_t)(id - (int32_t)OPTICS_UI_ICON_COUNT);
    if (index >= g_runtime_count)
        return nullptr;
    return &g_runtime_icons[index].desc;
}

uint8_t optics_ui__icon_mode(int32_t id) {
    if (id >= 0 && id < (int32_t)OPTICS_UI_ICON_COUNT)
        return optics_ui_icon_render_modes[id];
    if (id >= 0 && (uint32_t)(id - (int32_t)OPTICS_UI_ICON_COUNT) < g_runtime_count)
        return g_runtime_icons[id - (int32_t)OPTICS_UI_ICON_COUNT].mode;
    return OPTICS_UI_ICON_RENDER_STROKE;
}

OPTICS_UI_API const optics_ui_icon_desc *optics_ui_icon_info(optics_ui_icon_id icon,
                                                             uint8_t *mode) {
    const int32_t id = (int32_t)icon;
    if (!optics_ui__icon_valid(id))
        return nullptr;
    if (mode)
        *mode = optics_ui__icon_mode(id);
    return optics_ui__icon_desc(id);
}

optics_ui_icon_id optics_ui_icon_register_svg(const char *svg_text) {
    if (!svg_text)
        return OPTICS_UI_ICON_INVALID;

    size_t len = strlen(svg_text);
    if (len == 0 || len > OPTICS_UI_ICON_MAX_SVG_BYTES)
        return OPTICS_UI_ICON_INVALID;

    if (g_runtime_count >= OPTICS_UI_ICON_MAX_RUNTIME)
        return OPTICS_UI_ICON_INVALID;

    optics_svg_parse_opts parse_opts = {
        .text = svg_text,
        .length = len,
    };
    optics_svg_document *doc = nullptr;
    optics_gfx_result r = optics_svg_parse(&parse_opts, &doc);
    if (r != OPTICS_GFX_OK || !doc)
        return OPTICS_UI_ICON_INVALID;

    uint32_t shape_count = optics_svg_shape_count(doc);
    if (shape_count == 0) {
        optics_svg_release(doc);
        return OPTICS_UI_ICON_INVALID;
    }

    optics_gfx_rect vp = optics_svg_viewport(doc);
    float vb_w = vp.w > 0.0f ? vp.w : 24.0f;
    float vb_h = vp.h > 0.0f ? vp.h : 24.0f;
    float max_dim = fmaxf(vb_w, vb_h);
    if (max_dim <= 0.0f) {
        optics_svg_release(doc);
        return OPTICS_UI_ICON_INVALID;
    }
    float scale = 24.0f / max_dim;
    float ox = -vp.x * scale + (24.0f - vb_w * scale) * 0.5f;
    float oy = -vp.y * scale + (24.0f - vb_h * scale) * 0.5f;

    uint32_t total_cmds = 0;
    for (uint32_t i = 0; i < shape_count; ++i) {
        optics_svg_shape_view s = {};
        if (optics_svg_shape_at(doc, i, &s) == OPTICS_GFX_OK) {
            bool has_fill = (s.fill.kind != OPTICS_SVG_PAINT_NONE);
            bool has_stroke = (s.stroke.kind != OPTICS_SVG_PAINT_NONE);
            if ((has_fill || has_stroke) && s.cmd_count > 0) {
                total_cmds += s.cmd_count;
            }
        }
    }

    if (total_cmds == 0 || total_cmds > OPTICS_UI_ICON_MAX_CMDS) {
        optics_svg_release(doc);
        return OPTICS_UI_ICON_INVALID;
    }

    optics_ui_icon_cmd *cmds =
        (optics_ui_icon_cmd *)malloc(total_cmds * sizeof *cmds); // RAW_ALLOC_OK(process-registry)
    optics_ui_icon_run *runs =
        (optics_ui_icon_run *)malloc(shape_count * sizeof *runs); // RAW_ALLOC_OK(process-registry)
    if (!cmds || !runs) {
        free(cmds); // RAW_ALLOC_OK(process-registry)
        free(runs); // RAW_ALLOC_OK(process-registry)
        optics_svg_release(doc);
        return OPTICS_UI_ICON_INVALID;
    }

    uint32_t cur_cmd = 0;
    uint32_t run_count = 0;
    bool has_any_fill = false;

    for (uint32_t i = 0; i < shape_count; ++i) {
        optics_svg_shape_view s = {};
        if (optics_svg_shape_at(doc, i, &s) != OPTICS_GFX_OK)
            continue;

        bool has_fill = (s.fill.kind != OPTICS_SVG_PAINT_NONE);
        bool has_stroke = (s.stroke.kind != OPTICS_SVG_PAINT_NONE);
        if ((!has_fill && !has_stroke) || s.cmd_count == 0)
            continue;

        if (has_fill)
            has_any_fill = true;

        bool filled = has_fill; /* Fill takes precedence if both present */
        optics_svg_paint paint = filled ? s.fill : s.stroke;

        uint32_t raw_color = 0;
        if (paint.kind == OPTICS_SVG_PAINT_CURRENT_COLOR) {
            raw_color = 0u;
        } else if (paint.kind == OPTICS_SVG_PAINT_SOLID) {
            raw_color = paint.color;
            float total_opacity = s.opacity * paint.opacity;
            uint32_t a = (uint32_t)lrintf((float)((raw_color >> 24) & 0xFF) * total_opacity);
            if (a > 255)
                a = 255;
            raw_color = (a << 24) | (raw_color & 0x00FFFFFFu);
        } else if (paint.kind == OPTICS_SVG_PAINT_LINEAR_GRADIENT ||
                   paint.kind == OPTICS_SVG_PAINT_RADIAL_GRADIENT) {
            optics_svg_gradient_view g = {};
            if (optics_svg_gradient_at(doc, paint.gradient, &g) == OPTICS_GFX_OK &&
                g.stop_count > 0) {
                uint32_t premul = g.stops[0].color;
                uint8_t a = (uint8_t)(premul >> 24);
                uint8_t r = 0, gr = 0, b = 0;
                if (a > 0) {
                    r = (uint8_t)fminf(255.0f,
                                       roundf((float)((premul >> 16) & 0xFF) * 255.0f / (float)a));
                    gr = (uint8_t)fminf(255.0f,
                                        roundf((float)((premul >> 8) & 0xFF) * 255.0f / (float)a));
                    b = (uint8_t)fminf(255.0f, roundf((float)(premul & 0xFF) * 255.0f / (float)a));
                }
                float total_opacity = s.opacity * paint.opacity;
                uint32_t fa = (uint32_t)lrintf((float)a * total_opacity);
                if (fa > 255)
                    fa = 255;
                raw_color = (fa << 24) | ((uint32_t)r << 16) | ((uint32_t)gr << 8) | (uint32_t)b;
            } else {
                raw_color = 0u;
            }
        }

        uint32_t start_cmd = cur_cmd;
        for (uint32_t c = 0; c < s.cmd_count; ++c) {
            const optics_svg_path_cmd *pcmd = &s.cmds[c];
            optics_ui_icon_cmd *icmd = &cmds[cur_cmd++];
            *icmd = (optics_ui_icon_cmd){};
            switch (pcmd->verb) {
            case OPTICS_SVG_PATH_MOVE:
                icmd->type = 0;
                icmd->params[0] = pcmd->params[0] * scale + ox;
                icmd->params[1] = pcmd->params[1] * scale + oy;
                break;
            case OPTICS_SVG_PATH_LINE:
                icmd->type = 1;
                icmd->params[0] = pcmd->params[0] * scale + ox;
                icmd->params[1] = pcmd->params[1] * scale + oy;
                break;
            case OPTICS_SVG_PATH_QUAD:
                icmd->type = 3;
                icmd->params[0] = pcmd->params[0] * scale + ox;
                icmd->params[1] = pcmd->params[1] * scale + oy;
                icmd->params[2] = pcmd->params[2] * scale + ox;
                icmd->params[3] = pcmd->params[3] * scale + oy;
                break;
            case OPTICS_SVG_PATH_CUBIC:
                icmd->type = 2;
                icmd->params[0] = pcmd->params[0] * scale + ox;
                icmd->params[1] = pcmd->params[1] * scale + oy;
                icmd->params[2] = pcmd->params[2] * scale + ox;
                icmd->params[3] = pcmd->params[3] * scale + oy;
                icmd->params[4] = pcmd->params[4] * scale + ox;
                icmd->params[5] = pcmd->params[5] * scale + oy;
                break;
            case OPTICS_SVG_PATH_CLOSE:
                icmd->type = 4;
                break;
            default:
                break;
            }
        }

        uint32_t count = cur_cmd - start_cmd;
        if (count > 0) {
            if (run_count > 0 && runs[run_count - 1].color == raw_color &&
                runs[run_count - 1].fill == (uint8_t)filled &&
                runs[run_count - 1].first_cmd + runs[run_count - 1].count == start_cmd) {
                runs[run_count - 1].count += count;
            } else {
                runs[run_count++] = (optics_ui_icon_run){
                    .first_cmd = start_cmd,
                    .count = count,
                    .color = raw_color,
                    .fill = (uint8_t)filled,
                };
            }
        }
    }

    optics_svg_release(doc);

    if (cur_cmd == 0) {
        free(cmds); // RAW_ALLOC_OK(process-registry)
        free(runs); // RAW_ALLOC_OK(process-registry)
        return OPTICS_UI_ICON_INVALID;
    }

    bool can_collapse = true;
    for (uint32_t i = 0; i < run_count; ++i) {
        if (runs[i].color != 0 || runs[i].fill != runs[0].fill) {
            can_collapse = false;
            break;
        }
    }
    if (can_collapse) {
        free(runs); // RAW_ALLOC_OK(process-registry)
        runs = nullptr;
        run_count = 0;
    }

    if (g_runtime_count == g_runtime_cap) {
        uint32_t cap = g_runtime_cap ? g_runtime_cap * 2u : 8u;
        optics_ui__runtime_icon *grown =
            (optics_ui__runtime_icon *)realloc(        // RAW_ALLOC_OK(process-registry)
                g_runtime_icons, cap * sizeof *grown); // RAW_ALLOC_OK(process-registry)
        if (!grown) {
            free(cmds); // RAW_ALLOC_OK(process-registry)
            free(runs); // RAW_ALLOC_OK(process-registry)
            return OPTICS_UI_ICON_INVALID;
        }
        g_runtime_icons = grown;
        g_runtime_cap = cap;
    }

    g_runtime_icons[g_runtime_count] = (optics_ui__runtime_icon){
        .desc =
            {
                .cmds = cmds,
                .count = cur_cmd,
                .runs = runs,
                .run_count = run_count,
            },
        .mode = has_any_fill ? OPTICS_UI_ICON_RENDER_FILL : OPTICS_UI_ICON_RENDER_STROKE,
    };

    return (optics_ui_icon_id)((int32_t)OPTICS_UI_ICON_COUNT + (int32_t)g_runtime_count++);
}
