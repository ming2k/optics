/* icon_runtime.c — runtime SVG icon registration (lens_icon_register_svg).
 *
 * Built-in icons ship as generated lens_icon_cmd tables (icon_data.c); this
 * file lets applications register their own SVG assets at runtime and use
 * them through every icon widget exactly like a built-in.
 */

#include "icon_svg.h"
#include "internal.h"
#include <lens/icon.h>
#include <stdlib.h>
#include <string.h>

enum {
    LENSI_ICON_MAX_CMDS = 8192u,         /* per-icon verb cap */
    LENSI_ICON_MAX_RUNTIME = 4096u,      /* registry slot cap */
    LENSI_ICON_MAX_SVG_BYTES = 1u << 20, /* input sanity bound */
};

typedef struct lensi_runtime_icon {
    lens_icon_desc desc; /* cmds and runs owned by the entry, never freed */
    uint8_t mode;        /* LENSI_ICON_RENDER_* */
} lensi_runtime_icon;

/* Process-global registry: icons outlive any lens context. */
static lensi_runtime_icon *g_runtime_icons;
static uint32_t g_runtime_count;
static uint32_t g_runtime_cap;

bool lensi_icon_valid(int32_t id) {
    if (id < 0)
        return false;
    if (id < (int32_t)LENS_ICON_COUNT)
        return true;
    return (uint32_t)(id - (int32_t)LENS_ICON_COUNT) < g_runtime_count;
}

const lens_icon_desc *lensi_icon_desc(int32_t id) {
    if (id < 0)
        return nullptr;
    if (id < (int32_t)LENS_ICON_COUNT)
        return &lens_icon_table[id];
    uint32_t index = (uint32_t)(id - (int32_t)LENS_ICON_COUNT);
    if (index >= g_runtime_count)
        return nullptr;
    return &g_runtime_icons[index].desc;
}

uint8_t lensi_icon_mode(int32_t id) {
    if (id >= 0 && id < (int32_t)LENS_ICON_COUNT)
        return lens_icon_render_modes[id];
    if (id >= 0 && (uint32_t)(id - (int32_t)LENS_ICON_COUNT) < g_runtime_count)
        return g_runtime_icons[id - (int32_t)LENS_ICON_COUNT].mode;
    return LENSI_ICON_RENDER_STROKE;
}

LENS_API const lens_icon_desc *lens_icon_info(lens_icon_id icon, uint8_t *mode) {
    const int32_t id = (int32_t)icon;
    if (!lensi_icon_valid(id))
        return nullptr;
    if (mode)
        *mode = lensi_icon_mode(id);
    return lensi_icon_desc(id);
}

lens_icon_id lens_icon_register_svg(const char *svg_text) {
    if (!svg_text)
        return LENS_ICON_INVALID;

    size_t len = strlen(svg_text);
    if (len == 0 || len > LENSI_ICON_MAX_SVG_BYTES)
        return LENS_ICON_INVALID;

    if (g_runtime_count >= LENSI_ICON_MAX_RUNTIME)
        return LENS_ICON_INVALID;

    lensi_svg_result res = {0};
    if (!lensi_svg_parse(svg_text, &res))
        return LENS_ICON_INVALID;

    if (res.cmd_count == 0 || res.cmd_count > LENSI_ICON_MAX_CMDS) {
        free(res.cmds);
        free(res.runs);
        return LENS_ICON_INVALID;
    }

    if (g_runtime_count == g_runtime_cap) {
        uint32_t cap = g_runtime_cap ? g_runtime_cap * 2u : 8u;
        lensi_runtime_icon *grown =
            (lensi_runtime_icon *)realloc(g_runtime_icons, cap * sizeof *grown);
        if (!grown) {
            free(res.cmds);
            free(res.runs);
            return LENS_ICON_INVALID;
        }
        g_runtime_icons = grown;
        g_runtime_cap = cap;
    }

    g_runtime_icons[g_runtime_count] = (lensi_runtime_icon){
        .desc =
            {
                .cmds = res.cmds,
                .count = res.cmd_count,
                .runs = res.runs,
                .run_count = res.run_count,
            },
        .mode = res.has_fill ? LENSI_ICON_RENDER_FILL : LENSI_ICON_RENDER_STROKE,
    };

    return (lens_icon_id)((int32_t)LENS_ICON_COUNT + (int32_t)g_runtime_count++);
}
