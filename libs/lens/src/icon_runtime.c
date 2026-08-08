/* icon_runtime.c — runtime SVG icon registration (lens_icon_register_svg).
 *
 * Built-in icons ship as generated lens_icon_cmd tables (icon_data.c); this
 * file lets applications register their own SVG assets at runtime and use
 * them through every icon widget exactly like a built-in. The vendored
 * nanosvg parses the SVG, every visible shape is flattened to the same
 * move/cubic/close verb stream the built-ins use, and the glyph is
 * normalized into the shared 24x24 icon box at registration time so the
 * replay path applies verbatim. Runtime ids continue where the enum ends
 * (LENS_ICON_COUNT + index) and are never reclaimed.
 *
 * Two deliberate simplifications, both matching the built-in conventions:
 *  - Render mode is per icon, not per shape: any visible shape with a fill
 *    switches the whole icon to fill mode (like the material built-ins);
 *    stroke-only icons keep the feather 2/24 stroke convention. The
 *    source strokeWidth is not honoured — one weight keeps runtime icons
 *    visually interchangeable with the built-in set.
 *  - All shapes merge into a single flattened stream and replay paints it
 *    with one flux paint, so a per-shape fill rule cannot be expressed:
 *    fills always use nonzero winding (flux_paint_solid's default). The
 *    feather/material conventions this mirrors are nonzero-compatible;
 *    source SVGs that rely on even-odd holes will render with the holes
 *    filled. */

#include "internal.h"
#include "vendor/nanosvg.h"

enum {
    LENSI_ICON_MAX_CMDS = 8192u,         /* per-icon verb cap — bounds pathological assets */
    LENSI_ICON_MAX_RUNTIME = 4096u,      /* registry slot cap */
    LENSI_ICON_MAX_SVG_BYTES = 1u << 20, /* input sanity bound */
};

typedef struct lensi_runtime_icon {
    lens_icon_desc desc; /* cmds owned by the entry, never freed */
    uint8_t mode;        /* LENSI_ICON_RENDER_* */
} lensi_runtime_icon;

/* Process-global registry: icons outlive any lens context (ids handed out
 * must stay valid for the process), so this is plain libc malloc. */
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
        return NULL;
    if (id < (int32_t)LENS_ICON_COUNT)
        return &lens_icon_table[id];
    uint32_t index = (uint32_t)(id - (int32_t)LENS_ICON_COUNT);
    if (index >= g_runtime_count)
        return NULL;
    return &g_runtime_icons[index].desc;
}

uint8_t lensi_icon_mode(int32_t id) {
    if (id >= 0 && id < (int32_t)LENS_ICON_COUNT)
        return lens_icon_render_modes[id];
    if (id >= 0 && (uint32_t)(id - (int32_t)LENS_ICON_COUNT) < g_runtime_count)
        return g_runtime_icons[id - (int32_t)LENS_ICON_COUNT].mode;
    return LENSI_ICON_RENDER_STROKE;
}

LENS_API lens_icon_id lens_icon_register_svg(const char *svg_utf8) {
    if (!svg_utf8 || g_runtime_count >= LENSI_ICON_MAX_RUNTIME)
        return LENS_ICON_INVALID;
    size_t bytes = strlen(svg_utf8) + 1;
    if (bytes > LENSI_ICON_MAX_SVG_BYTES)
        return LENS_ICON_INVALID;

    /* nsvgParse edits the input in place — hand it a private copy. */
    char *buf = (char *)malloc(bytes);
    if (!buf)
        return LENS_ICON_INVALID;
    memcpy(buf, svg_utf8, bytes);
    NSVGimage *img = nsvgParse(buf, "px", 96.0f);
    free(buf);
    /* nanosvg returns an empty image (not NULL) for non-SVG input; both
     * are invalid icons. A zero/NaN box gives no usable coordinate frame. */
    if (!img || !img->shapes || !(img->width > 0.0f) || !(img->height > 0.0f)) {
        nsvgDelete(img); /* null-safe */
        return LENS_ICON_INVALID;
    }

    /* Pass 1: count the flattened verbs and pick the render mode. */
    uint32_t count = 0;
    bool has_fill = false;
    for (const NSVGshape *s = img->shapes; s; s = s->next) {
        if (!(s->flags & NSVG_FLAGS_VISIBLE))
            continue;
        if (s->fill.type != NSVG_PAINT_NONE)
            has_fill = true;
        for (const NSVGpath *p = s->paths; p; p = p->next) {
            if (p->npts < 4)
                continue; /* degenerate: fewer points than one cubic segment */
            uint32_t segs = (uint32_t)(p->npts - 1) / 3u;
            count += 1u + segs + (p->closed ? 1u : 0u);
            if (count > LENSI_ICON_MAX_CMDS) {
                nsvgDelete(img);
                return LENS_ICON_INVALID;
            }
        }
    }
    if (count == 0) {
        nsvgDelete(img);
        return LENS_ICON_INVALID;
    }

    /* Normalize into the shared 24x24 icon box: uniform scale from the
     * longer axis, centred on the shorter one. */
    const float scale = 24.0f / fmaxf(img->width, img->height);
    const float ox = (24.0f - img->width * scale) * 0.5f;
    const float oy = (24.0f - img->height * scale) * 0.5f;

    lens_icon_cmd *cmds = (lens_icon_cmd *)malloc(count * sizeof *cmds);
    if (!cmds) {
        nsvgDelete(img);
        return LENS_ICON_INVALID;
    }

    /* Pass 2: emit. nanosvg yields cubic segments only — lines arrive as
     * degenerate cubics, which tessellate identically. */
    uint32_t n = 0;
    bool bad = false;
    for (const NSVGshape *s = img->shapes; s && !bad; s = s->next) {
        if (!(s->flags & NSVG_FLAGS_VISIBLE))
            continue;
        for (const NSVGpath *p = s->paths; p && !bad; p = p->next) {
            if (p->npts < 4)
                continue;
            const float *pts = p->pts;
            for (int i = 0; i < p->npts * 2; i++) {
                if (!isfinite(pts[i])) {
                    bad = true; /* reject NaN/Inf coordinates wholesale */
                    break;
                }
            }
            if (bad)
                break;
            cmds[n++] = (lens_icon_cmd){
                .type = 0, .params = {pts[0] * scale + ox, pts[1] * scale + oy}};
            float cx = pts[0] * scale + ox;
            float cy = pts[1] * scale + oy;
            for (int i = 1; i + 2 < p->npts; i += 3) {
                const float *q = &pts[i * 2];
                float ex = q[4] * scale + ox;
                float ey = q[5] * scale + oy;
                /* nanosvg's element conversions (circle, arcs) emit trailing
                 * zero-length cubics. Skip them: they carry no ink, and the
                 * flattener must never be handed an all-coincident cubic. */
                float d1 = (q[0] * scale + ox - cx) * (q[0] * scale + ox - cx) +
                           (q[1] * scale + oy - cy) * (q[1] * scale + oy - cy);
                float d2 = (q[2] * scale + ox - cx) * (q[2] * scale + ox - cx) +
                           (q[3] * scale + oy - cy) * (q[3] * scale + oy - cy);
                float d3 = (ex - cx) * (ex - cx) + (ey - cy) * (ey - cy);
                float farthest = fmaxf(d1, fmaxf(d2, d3));
                if (farthest < 1e-8f)
                    continue;
                cmds[n++] = (lens_icon_cmd){.type = 2,
                                            .params = {q[0] * scale + ox, q[1] * scale + oy,
                                                       q[2] * scale + ox, q[3] * scale + oy,
                                                       ex, ey}};
                cx = ex;
                cy = ey;
            }
            if (p->closed)
                cmds[n++] = (lens_icon_cmd){.type = 4};
        }
    }
    nsvgDelete(img);
    if (bad || n == 0) {
        free(cmds);
        return LENS_ICON_INVALID;
    }

    if (g_runtime_count == g_runtime_cap) {
        uint32_t cap = g_runtime_cap ? g_runtime_cap * 2u : 8u;
        lensi_runtime_icon *grown =
            (lensi_runtime_icon *)realloc(g_runtime_icons, cap * sizeof *grown);
        if (!grown) {
            free(cmds);
            return LENS_ICON_INVALID;
        }
        g_runtime_icons = grown;
        g_runtime_cap = cap;
    }
    g_runtime_icons[g_runtime_count] = (lensi_runtime_icon){
        .desc = {.cmds = cmds, .count = n},
        .mode = has_fill ? LENSI_ICON_RENDER_FILL : LENSI_ICON_RENDER_STROKE,
    };
    return (lens_icon_id)((int32_t)LENS_ICON_COUNT + (int32_t)g_runtime_count++);
}
