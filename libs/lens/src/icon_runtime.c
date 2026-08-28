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
 * Paint model: registration records one lens_icon_run per source shape —
 * its colour (explicit fill/stroke colours are preserved; currentColor and
 * unpainted shapes record color == 0, "follow the theme") and its mode
 * (fill vs stroke). Icons whose every run is theme-coloured collapse to
 * runs == NULL, which replays exactly like a built-in: one theme paint.
 * Two deliberate simplifications, both matching the built-in conventions:
 *  - The source strokeWidth is not honoured — one weight keeps runtime
 *    icons visually interchangeable with the built-in set.
 *  - A per-shape fill *rule* cannot be expressed: fills always use
 *    nonzero winding (flux_paint_solid's default). The feather/material
 *    conventions this mirrors are nonzero-compatible; source SVGs that
 *    rely on even-odd holes will render with the holes filled. */

#include "internal.h"
#include "vendor/nanosvg.h"
#include <lens/icon.h>
#include <string.h>

/* "currentColor" has no nanosvg representation (it parses as a named
 * colour and lands on fallback grey). lens rewrites the token to this
 * sentinel hex before parsing; a paint that comes back as exactly this
 * colour means "follow the theme colour" and the run is recorded with
 * color == 0 (see lens_icon_run). nanosvg packs colours 0xBBGGRR
 * (NSVG_RGB), so #010203 arrives as 0x030201. The same hex appearing
 * verbatim in a source SVG would degrade to a near-black paint — the cost
 * of a text-level rewrite; the alternative is forking the parser. */
#define LENSI_CURRENTCOLOR_SENTINEL_BGR 0x030201u
#define LENSI_CURRENTCOLOR_HEX "#010203"

enum {
    LENSI_ICON_MAX_CMDS = 8192u,         /* per-icon verb cap — bounds pathological assets */
    LENSI_ICON_MAX_RUNTIME = 4096u,      /* registry slot cap */
    LENSI_ICON_MAX_SVG_BYTES = 1u << 20, /* input sanity bound */
    LENSI_ICON_MAX_RUNS = 256u,          /* per-icon paint-run cap */
};

typedef struct lensi_runtime_icon {
    lens_icon_desc desc; /* cmds and runs owned by the entry, never freed */
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

LENS_API const lens_icon_desc *lens_icon_info(lens_icon_id icon, uint8_t *mode) {
    const int32_t id = (int32_t)icon;
    if (!lensi_icon_valid(id))
        return NULL;
    if (mode)
        *mode = lensi_icon_mode(id);
    return lensi_icon_desc(id);
}

/* Rewrite every `currentColor` token (as a fill/stroke attribute value) in
 * the private copy to the sentinel hex so nanosvg can carry it through as
 * a colour we recognise afterwards. The replacement is shorter than the
 * token, so the tail shifts left in one pass. Operates on the
 * NUL-terminated copy only — the caller's string is never touched. */
static void rewrite_currentcolor(char *svg) {
    static const char token[] = "currentColor";
    const size_t token_len = sizeof token - 1;
    static const char hex[] = LENSI_CURRENTCOLOR_HEX;
    const size_t hex_len = sizeof hex - 1;
    char *at = svg;
    while ((at = strstr(at, token)) != NULL) {
        bool quoted_before = at > svg && (at[-1] == '"' || at[-1] == '\'');
        char after = at[token_len];
        bool quoted_after = after == '"' || after == '\'';
        if (quoted_before && quoted_after) {
            /* exact attribute value: replace and shift the tail left */
            memmove(at + hex_len, at + token_len, strlen(at + token_len) + 1);
            memcpy(at, hex, hex_len);
            at += hex_len;
        } else {
            at += token_len;
        }
    }
}

/* Straight 0xRRGGBBAA run colour from an NSVGpaint (nanosvg packs colours
 * as 0xBBGGRR with alpha in the top byte), or 0 when the paint carries no
 * explicit colour (none / currentColor sentinel -> theme colour). */
static uint32_t paint_run_color(const NSVGpaint *p) {
    if (p->type == NSVG_PAINT_COLOR) {
        uint32_t c = p->color;
        if ((c & 0xFFFFFFu) == LENSI_CURRENTCOLOR_SENTINEL_BGR)
            return 0; /* theme colour */
        uint32_t r = c & 0xFFu;
        uint32_t g = (c >> 8) & 0xFFu;
        uint32_t b = (c >> 16) & 0xFFu;
        uint32_t a = (c >> 24) & 0xFFu;
        return (r << 16) | (g << 8) | b | (a << 24);
    }
    if (p->type == NSVG_PAINT_LINEAR_GRADIENT || p->type == NSVG_PAINT_RADIAL_GRADIENT) {
        /* Degrade a gradient to its first stop colour; nanosvg guarantees
         * nstops >= 1 on parsed gradients. */
        const NSVGgradient *g = p->gradient;
        if (g && g->nstops > 0) {
            uint32_t s = g->stops[0].color;
            uint32_t r = s & 0xFFu;
            uint32_t gg = (s >> 8) & 0xFFu;
            uint32_t b = (s >> 16) & 0xFFu;
            uint32_t a = (s >> 24) & 0xFFu;
            return (r << 16) | (gg << 8) | b | (a << 24);
        }
    }
    return 0; /* NSVG_PAINT_NONE / UNDEF: theme colour */
}

LENS_API lens_icon_id lens_icon_register_svg(const char *svg_utf8) {
    if (!svg_utf8 || g_runtime_count >= LENSI_ICON_MAX_RUNTIME)
        return LENS_ICON_INVALID;
    size_t bytes = strlen(svg_utf8) + 1;
    if (bytes > LENSI_ICON_MAX_SVG_BYTES)
        return LENS_ICON_INVALID;

    /* nsvgParse edits the input in place — hand it a private copy, with
     * currentColor already rewritten to the sentinel hex. */
    char *buf = (char *)malloc(bytes);
    if (!buf)
        return LENS_ICON_INVALID;
    memcpy(buf, svg_utf8, bytes);
    rewrite_currentcolor(buf);
    NSVGimage *img = nsvgParse(buf, "px", 96.0f);
    free(buf);
    /* nanosvg returns an empty image (not NULL) for non-SVG input; both
     * are invalid icons. A zero/NaN box gives no usable coordinate frame. */
    if (!img || !img->shapes || !(img->width > 0.0f) || !(img->height > 0.0f)) {
        nsvgDelete(img); /* null-safe */
        return LENS_ICON_INVALID;
    }

    /* Pass 1: count the flattened verbs and the paint runs. A shape with
     * both fill and stroke contributes one fill run (documented contract);
     * a stroke-only shape contributes one stroke run. Shapes with no
     * geometry after flattening contribute nothing. */
    uint32_t count = 0;
    uint32_t run_count = 0;
    bool has_fill = false;
    bool any_explicit_color = false;
    for (const NSVGshape *s = img->shapes; s; s = s->next) {
        if (!(s->flags & NSVG_FLAGS_VISIBLE))
            continue;
        bool shape_filled = s->fill.type != NSVG_PAINT_NONE;
        bool shape_stroked = s->stroke.type != NSVG_PAINT_NONE;
        uint32_t shape_cmds = 0;
        for (const NSVGpath *p = s->paths; p; p = p->next) {
            if (p->npts < 4)
                continue; /* degenerate: fewer points than one cubic segment */
            uint32_t segs = (uint32_t)(p->npts - 1) / 3u;
            shape_cmds += 1u + segs + (p->closed ? 1u : 0u);
            if (shape_cmds > LENSI_ICON_MAX_CMDS) {
                nsvgDelete(img);
                return LENS_ICON_INVALID;
            }
        }
        if (shape_cmds == 0)
            continue;
        count += shape_cmds;
        if (shape_filled || shape_stroked) {
            if (run_count >= LENSI_ICON_MAX_RUNS) {
                nsvgDelete(img);
                return LENS_ICON_INVALID;
            }
            uint32_t color =
                paint_run_color(shape_filled ? &s->fill : &s->stroke);
            if (color != 0)
                any_explicit_color = true;
            if (shape_filled)
                has_fill = true;
            run_count++;
        }
    }
    if (count == 0) {
        nsvgDelete(img);
        return LENS_ICON_INVALID;
    }
    /* No explicit colours anywhere: collapse to the built-in single-paint
     * representation (runs == NULL), so replay and hashing behave exactly
     * like a built-in icon. */
    if (!any_explicit_color)
        run_count = 0;

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
    lens_icon_run *runs = NULL;
    if (run_count > 0) {
        runs = (lens_icon_run *)malloc(run_count * sizeof *runs);
        if (!runs) {
            free(cmds);
            nsvgDelete(img);
            return LENS_ICON_INVALID;
        }
    }

    /* Pass 2: emit. nanosvg yields cubic segments only — lines arrive as
     * degenerate cubics, which tessellate identically. One run per shape
     * brackets the shape's verbs; consecutive shapes with identical
     * (color, fill) merge into a single run so the replay path flushes as
     * few paints as possible. */
    uint32_t n = 0;
    uint32_t nruns = 0;
    bool bad = false;
    for (const NSVGshape *s = img->shapes; s && !bad; s = s->next) {
        if (!(s->flags & NSVG_FLAGS_VISIBLE))
            continue;
        bool shape_filled = s->fill.type != NSVG_PAINT_NONE;
        bool shape_stroked = s->stroke.type != NSVG_PAINT_NONE;
        uint32_t shape_first = n;
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
            cmds[n++] =
                (lens_icon_cmd){.type = 0, .params = {pts[0] * scale + ox, pts[1] * scale + oy}};
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
                cmds[n++] =
                    (lens_icon_cmd){.type = 2,
                                    .params = {q[0] * scale + ox, q[1] * scale + oy,
                                               q[2] * scale + ox, q[3] * scale + oy, ex, ey}};
                cx = ex;
                cy = ey;
            }
            if (p->closed)
                cmds[n++] = (lens_icon_cmd){.type = 4};
        }
        if (bad || n == shape_first)
            continue; /* no geometry survived for this shape */
        if (runs && (shape_filled || shape_stroked)) {
            bool fill = shape_filled;
            uint32_t color = paint_run_color(shape_filled ? &s->fill : &s->stroke);
            if (nruns > 0 && runs[nruns - 1].color == color &&
                runs[nruns - 1].fill == (uint8_t)fill &&
                runs[nruns - 1].first_cmd + runs[nruns - 1].count == shape_first) {
                /* Extend the previous run to cover this shape too. */
                runs[nruns - 1].count = n - runs[nruns - 1].first_cmd;
            } else {
                runs[nruns++] = (lens_icon_run){
                    .first_cmd = shape_first,
                    .count = n - shape_first,
                    .color = color,
                    .fill = (uint8_t)fill,
                };
            }
        }
    }
    nsvgDelete(img);
    if (bad || n == 0) {
        free(cmds);
        free(runs);
        return LENS_ICON_INVALID;
    }
    if (nruns == 0) {
        free(runs); /* every shape theme-coloured after all */
        runs = NULL;
    }

    if (g_runtime_count == g_runtime_cap) {
        uint32_t cap = g_runtime_cap ? g_runtime_cap * 2u : 8u;
        lensi_runtime_icon *grown =
            (lensi_runtime_icon *)realloc(g_runtime_icons, cap * sizeof *grown);
        if (!grown) {
            free(cmds);
            free(runs);
            return LENS_ICON_INVALID;
        }
        g_runtime_icons = grown;
        g_runtime_cap = cap;
    }
    g_runtime_icons[g_runtime_count] = (lensi_runtime_icon){
        .desc = {.cmds = cmds, .count = n, .runs = runs, .run_count = nruns},
        .mode = has_fill ? LENSI_ICON_RENDER_FILL : LENSI_ICON_RENDER_STROKE,
    };
    return (lens_icon_id)((int32_t)LENS_ICON_COUNT + (int32_t)g_runtime_count++);
}
