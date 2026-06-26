/* layout.c — the single text-layout path: itemise -> shape -> position into a
 * placed-glyph list, consumed by measure, draw and caret mapping so all three
 * agree on geometry.
 *
 * Phase 1 keeps the legacy grid (shape at round(size_px), rasterise at
 * round(size_px*scale)) and integer-snaps each glyph at draw time; subpixel
 * positioning replaces the snapping in a later phase. */

#include "text_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Shaping                                                            */
/* ------------------------------------------------------------------ */

static void shape_run(flux_text *t, const text_run *run) {
    hb_buffer_reset(t->hb_buf);
    hb_buffer_add_utf8(t->hb_buf, run->text, (int)run->len, 0, (int)run->len);
    /* Direction and script come from BiDi/itemisation, not a guess, so mixed
     * and RTL text shapes and orders correctly. */
    hb_buffer_set_direction(t->hb_buf, run->rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
    if (run->script != HB_SCRIPT_INVALID && run->script != HB_SCRIPT_UNKNOWN)
        hb_buffer_set_script(t->hb_buf, run->script);
    hb_buffer_set_language(t->hb_buf, hb_language_get_default());
    txt_face *f = &t->slots[run->slot_idx].faces[run->face_idx];
    hb_shape(f->hb_font, t->hb_buf, NULL, 0);
}

/* ------------------------------------------------------------------ */
/*  Inter-script auto-space                                            */
/* ------------------------------------------------------------------ */

/* Whitespace a user might type between a CJK and a Latin run. When one
 * already sits at a script boundary the auto-space is suppressed — the
 * typed space already provides the visual breath, and we must not double
 * it. Covers ASCII whitespace, NBSP, and the ideographic space (U+3000)
 * a CJK IME commonly emits. */
static inline bool txt_boundary_is_separator(FcChar32 cp) {
    return cp == 0x20 || (cp >= 0x09 && cp <= 0x0d) /* \t \n \v \f \r */
           || cp == 0xa0                            /* NO-BREAK SPACE */
           || cp == 0x3000;                         /* IDEOGRAPHIC SPACE */
}

/* A thin (~1/4 em) gap inserted at a boundary between a CJK and a non-CJK
 * run so "你好hello" renders with a visible breath instead of glued glyphs.
 * Standard CJK typography — CSS `text-autospace`, ctex/xeCJK — treats this
 * as the default.
 *
 * Because the gap is added to the layout pen (no glyph, no source cluster),
 * measure / draw / caret / selection all see it consistently: the empty
 * quarter-em simply falls between the runs' glyphs.
 *
 * Returns the gap in EM units (caller scales by size_px), or 0 when:
 *   - the two runs are not on opposite sides of the CJK line, or
 *   - a separator space already sits at the boundary (no doubling). */
#define TXT_INTERSCRIPT_GAP_EM 0.25f
static float txt_run_autospace_em(const text_run *prev, const text_run *cur) {
    if (!prev || !cur || prev->len == 0 || cur->len == 0)
        return 0.0f;
    if (txt_script_is_cjk(prev->script) == txt_script_is_cjk(cur->script))
        return 0.0f;

    /* Last codepoint of the previous run: back up to its lead byte. */
    FcChar32 cp = 0;
    size_t off = prev->len;
    while (off > 0) {
        --off;
        if (((unsigned char)prev->text[off] & 0xc0) != 0x80)
            break;
    }
    txt_utf8_decode(prev->text + off, prev->len - off, &cp);
    if (txt_boundary_is_separator(cp))
        return 0.0f;

    /* First codepoint of the current run. */
    txt_utf8_decode(cur->text, cur->len, &cp);
    if (txt_boundary_is_separator(cp))
        return 0.0f;

    return TXT_INTERSCRIPT_GAP_EM;
}

/* CJK full-width bracket compression (≈ JLREQ "prefixed/postfixed" glyph
 * classes). A full-width opening bracket carries its ink in the leading
 * half of its em-box and an empty trailing half; a closing bracket the
 * reverse. Compression trims the empty ½-em half when a neighbour is
 * present, so brackets attach to their content instead of leaving a
 * half-em gap — the most visible piece of "pro" CJK typography.
 *
 * (Stop punctuation 。，、… is intentionally not compressed here: its ink
 * is corner-placed in a font-dependent way, so trimming a fixed half
 * requires a per-glyph policy that belongs in a later pass, not the
 * shape-time advance trim these helpers drive.) */
static inline bool txt_is_opening_bracket(FcChar32 cp) {
    return cp == 0x3008 || cp == 0x300A || cp == 0x300C || cp == 0x300E || cp == 0x3010 ||
           cp == 0x3014 || cp == 0x3016 || cp == 0x301A || cp == 0x301D || cp == 0xFF08 ||
           cp == 0xFF3B || cp == 0xFF5B || cp == 0xFF5F;
}

static inline bool txt_is_closing_bracket(FcChar32 cp) {
    return cp == 0x3009 || cp == 0x300B || cp == 0x300D || cp == 0x300F || cp == 0x3011 ||
           cp == 0x3015 || cp == 0x3017 || cp == 0x301B || cp == 0x301E || cp == 0xFF09 ||
           cp == 0xFF3D || cp == 0xFF5D || cp == 0xFF60;
}

/* ------------------------------------------------------------------ */
/*  Placed-glyph buffer                                                */
/* ------------------------------------------------------------------ */

static bool layout_reserve(flux_text *t, int need) {
    if (need <= t->layout_cap)
        return true;
    int cap = t->layout_cap ? t->layout_cap * 2 : 64;
    if (cap < need)
        cap = need;
    txt_placed_glyph *p = realloc(t->layout_buf, (size_t)cap * sizeof *p);
    if (!p)
        return false;
    t->layout_buf = p;
    t->layout_cap = cap;
    return true;
}

/* ------------------------------------------------------------------ */
/*  Build                                                              */
/* ------------------------------------------------------------------ */

bool txt_text_layout_build(flux_text *t, const char *utf8, size_t len, float size_px, float weight,
                           bool italic, flux_text_family family, float scale,
                           txt_text_layout *out) {
    memset(out, 0, sizeof *out);
    if (!t || !utf8 || len == 0 || size_px <= 0.0f)
        return false;
    if (scale <= 0.0f)
        scale = 1.0f;

    family = txt_resolve_family(t, family);

    /* Layout cache: an LRU cache of recently shaped strings. Avoids
     * rebuilding the glyph list on every frame for static UI elements.
     * The key is a content fingerprint (string content + style + scale). */
    int best_slot = 0;
    uint32_t oldest = 0xFFFFFFFF;
    t->layout_cache_tick++;

    for (int i = 0; i < 32; i++) {
        if (t->layout_cache[i].last_used == 0) {
            best_slot = i;
            oldest = 0;
        } else if (t->layout_cache[i].last_used < oldest) {
            oldest = t->layout_cache[i].last_used;
            best_slot = i;
        }

        if (t->layout_cache[i].last_used > 0 && t->layout_cache[i].len == len &&
            t->layout_cache[i].size_px == size_px && t->layout_cache[i].weight == weight &&
            t->layout_cache[i].italic == italic && t->layout_cache[i].family == family &&
            t->layout_cache[i].scale == scale && memcmp(t->layout_cache[i].utf8, utf8, len) == 0) {

            t->layout_cache[i].last_used = t->layout_cache_tick;
            *out = t->layout_cache[i].layout;
            return out->count > 0;
        }
    }

    int slot_idx = txt_family_to_slot(family, weight, italic);
    if (t->slots[slot_idx].count == 0 && !txt_init_slot_faces(t, slot_idx, weight, italic, family))
        slot_idx = 0;
    if (t->slots[slot_idx].count == 0)
        return false;

    /* Shape and rasterise on a single grid — the device-pixel size — so glyph
     * advances and bitmaps share one coordinate system; advances are then
     * scaled back to logical px. */
    uint32_t rpx = (uint32_t)lroundf(size_px * scale);
    if (rpx == 0)
        rpx = 1;
    float inv_scale = 1.0f / scale;

    out->scale = scale;
    out->inv_scale = inv_scale;
    out->rpx = rpx;

    int n_runs = txt_itemize(t, slot_idx, utf8, len);

    float pen = 0.0f; /* logical px */
    float max_ascent = 0.0f;
    float max_descent = 0.0f;
    int count = 0;

    for (int i = 0; i < n_runs; i++) {
        const text_run *run = &t->runs_buf[i];
        /* Inter-script auto-space: nudge the pen a quarter-em right at a
         * CJK<->non-CJK boundary when no separator already covers it. The
         * gap carries no glyph or cluster, so it renders and measures as
         * empty space between the runs and caret/selection skip over it. */
        if (i > 0) {
            float gap_em = txt_run_autospace_em(&t->runs_buf[i - 1], run);
            if (gap_em > 0.0f)
                pen += gap_em * size_px;
        }
        if (!txt_ensure_face_loaded(t, run->slot_idx, run->face_idx, rpx))
            continue;
        shape_run(t, run);

        unsigned int n = 0;
        hb_glyph_info_t *info = hb_buffer_get_glyph_infos(t->hb_buf, &n);
        hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(t->hb_buf, &n);
        if (!n)
            continue;

        txt_face *f = &t->slots[run->slot_idx].faces[run->face_idx];
        float asc = size_px * f->ascent_em;
        float desc = size_px * f->descent_em;
        if (asc > max_ascent)
            max_ascent = asc;
        if (desc > max_descent)
            max_descent = desc;

        if (!layout_reserve(t, count + (int)n))
            return false;
        int face_id = encode_face_id(run->slot_idx, run->face_idx);

        for (unsigned j = 0; j < n; j++) {
            txt_placed_glyph *g = &t->layout_buf[count++];
            g->face_id = face_id;
            g->gid = info[j].codepoint;
            g->x = pen + (float)pos[j].x_offset / 64.0f * inv_scale;
            g->y_off = (float)pos[j].y_offset / 64.0f * inv_scale;
            g->advance = (float)pos[j].x_advance / 64.0f * inv_scale;
            g->cluster = (int)(run->byte_off + info[j].cluster);
            g->phase = 0; /* assigned at draw time from the real pen frac */
            g->rtl = run->rtl;
            g->subpixel = !txt_script_is_cjk(run->script);
            pen += g->advance;
        }
    }

    /* Full-width CJK bracket compression: trim the empty ½-em half of an
     * opening bracket's trailing edge (when followed) and a closing
     * bracket's leading edge (when preceded), so brackets attach to their
     * content. Applied as delta shifts so each glyph's HarfBuzz x_offset
     * and the inter-script run gaps already baked into g->x are preserved.
     *
     * Closing bracket at k pulls itself left by ½ em (trimming its leading
     * half) — modelled as a reduction of the previous glyph's advance plus
     * a leftward shift of glyph k and everything after. Opening bracket at
     * k trims its own trailing ½ em — a reduction of g->advance plus a
     * leftward shift of everything after k. The two accumulate, so e.g.
     * "（）" collapses to a single em (each bracket half-width). */
    if (count > 1) {
        const float half = 0.5f * size_px;
        float shift = 0.0f;
        for (int k = 0; k < count; k++) {
            txt_placed_glyph *g = &t->layout_buf[k];
            g->x -= shift;
            FcChar32 cp = 0;
            size_t off = (size_t)g->cluster;
            if (off < len)
                txt_utf8_decode(utf8 + off, len - off, &cp);
            if (k > 0 && txt_is_closing_bracket(cp)) {
                t->layout_buf[k - 1].advance -= half;
                g->x -= half;
                shift += half;
            }
            if (k + 1 < count && txt_is_opening_bracket(cp)) {
                g->advance -= half;
                shift += half;
            }
        }
        /* Width is the trailing edge of the last glyph. */
        pen = t->layout_buf[count - 1].x + t->layout_buf[count - 1].advance;
    }

    out->glyphs = t->layout_buf;
    out->count = count;
    out->width = pen;
    out->baseline = max_ascent;
    out->height = max_ascent + max_descent;
    if (count > 0) {
        /* Populate the cache slot. Allocate and copy the utf8 string and the
         * glyph list so they outlive the caller and t->layout_buf. */
        if (t->layout_cache[best_slot].cap < len) {
            char *p = realloc(t->layout_cache[best_slot].utf8, len);
            if (p) {
                t->layout_cache[best_slot].utf8 = p;
                t->layout_cache[best_slot].cap = len;
            } else {
                return true; /* fail gracefully (don't cache) */
            }
        }
        memcpy(t->layout_cache[best_slot].utf8, utf8, len);

        if (t->layout_cache[best_slot].layout_cap < count) {
            txt_placed_glyph *p =
                realloc(t->layout_cache[best_slot].layout_buf, count * sizeof(txt_placed_glyph));
            if (p) {
                t->layout_cache[best_slot].layout_buf = p;
                t->layout_cache[best_slot].layout_cap = count;
            } else {
                return true;
            }
        }
        memcpy(t->layout_cache[best_slot].layout_buf, t->layout_buf,
               count * sizeof(txt_placed_glyph));

        t->layout_cache[best_slot].len = len;
        t->layout_cache[best_slot].size_px = size_px;
        t->layout_cache[best_slot].weight = weight;
        t->layout_cache[best_slot].italic = italic;
        t->layout_cache[best_slot].family = family;
        t->layout_cache[best_slot].scale = scale;

        t->layout_cache[best_slot].layout = *out;
        t->layout_cache[best_slot].layout.glyphs = t->layout_cache[best_slot].layout_buf;
        t->layout_cache[best_slot].last_used = t->layout_cache_tick;

        /* Point out->glyphs to the cached buffer so it outlives this call. */
        out->glyphs = t->layout_cache[best_slot].layout_buf;
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/*  Style / scale                                                      */
/* ------------------------------------------------------------------ */

#define TXT_DEFAULT_SIZE_PX 16.0f

/* Unpack a (possibly NULL) public style into the scalars the engine uses.
 * NULL or non-positive size falls back to the context default. The family is
 * resolved against the context default (DEFAULT -> sans-serif at creation). */
static void style_unpack(flux_text *t, const flux_text_style *s, float *size_px, float *weight,
                         bool *italic, flux_text_family *family) {
    *size_px = (s && s->size_px > 0.0f) ? s->size_px : TXT_DEFAULT_SIZE_PX;
    *weight = s ? s->weight : 0.0f;
    *italic = s ? s->italic : false;
    *family = txt_resolve_family(t, s ? s->family : FLUX_TEXT_FAMILY_DEFAULT);
}

void flux_text_set_scale(flux_text *t, float scale) {
    if (!t)
        return;
    float s = (scale > 0.0f) ? scale : 1.0f;
    /* Scale participates in the layout cache key, so a change invalidates. */
    if (t->scale != s) {
        for (int i = 0; i < 32; i++)
            t->layout_cache[i].last_used = 0;
    }
    t->scale = s;
}

float flux_text_scale(const flux_text *t) {
    return t ? t->scale : 1.0f;
}

flux_text_family flux_text_default_family(const flux_text *t) {
    return t ? t->default_family : FLUX_TEXT_FAMILY_SANS;
}

void flux_text_set_default_family(flux_text *t, flux_text_family family) {
    if (!t)
        return;
    /* Normalise DEFAULT/invalid to a concrete family before storing. */
    switch (family) {
    case FLUX_TEXT_FAMILY_SANS:
    case FLUX_TEXT_FAMILY_SERIF:
    case FLUX_TEXT_FAMILY_MONO:
        if (t->default_family != family) {
            for (int i = 0; i < 32; i++)
                t->layout_cache[i].last_used = 0;
        }
        t->default_family = family;
        break;
    default:
        if (t->default_family != FLUX_TEXT_FAMILY_SANS) {
            for (int i = 0; i < 32; i++)
                t->layout_cache[i].last_used = 0;
        }
        t->default_family = FLUX_TEXT_FAMILY_SANS;
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  Measure                                                            */
/* ------------------------------------------------------------------ */

flux_text_metrics flux_text_measure(flux_text *t, const char *utf8, size_t len,
                                    const flux_text_style *style) {
    flux_text_metrics m = {0};
    if (!t || !utf8 || len == 0)
        return m;

    float size_px, weight;
    bool italic;
    flux_text_family family;
    style_unpack(t, style, &size_px, &weight, &italic, &family);

    if (!t->has_backend)
        return txt_text_measure_mono(utf8, len, size_px, weight);

    txt_text_layout L;
    if (!txt_text_layout_build(t, utf8, len, size_px, weight, italic, family, 1.0f, &L))
        return m;
    m.width = L.width;
    m.height = L.height;
    m.baseline = L.baseline;
    return m;
}

/* ------------------------------------------------------------------ */
/*  Draw                                                               */
/* ------------------------------------------------------------------ */

/* Quads per flux_canvas_draw_glyph_run flush. Each flush is one draw
 * call; a typical label fits in one flush, long paragraphs take a
 * handful. Stack-resident so the hot path never allocates. */
#define TEXT_RUN_BATCH 256

void flux_text_draw(flux_text *t, flux_canvas *canvas, flux_arena *arena, float x, float y,
                    const char *utf8, size_t len, const flux_text_style *style) {
    (void)arena;
    if (!t || !canvas || !t->has_backend || !t->atlas || !utf8 || len == 0)
        return;

    float size_px, weight;
    bool italic;
    flux_text_family family;
    style_unpack(t, style, &size_px, &weight, &italic, &family);
    flux_color color = style ? style->color : (flux_color){0};
    /* The canvas owns the content scale; rasterise glyphs to match it so the
     * canvas's base transform maps the logical quads onto physical pixels
     * crisply. Fall back to the context's own scale (measure-only contexts,
     * or a canvas left at the default). */
    float scale = flux_canvas_get_scale(canvas);
    if (scale <= 0.0f)
        scale = (t->scale > 0.0f) ? t->scale : 1.0f;

    txt_text_layout L;
    if (!txt_text_layout_build(t, utf8, len, size_px, weight, italic, family, scale, &L))
        return;

    float inv_scale = L.inv_scale;

    /* Baseline snaps once to an integer device row (no vertical subpixel);
     * every glyph shares it so mixed-face runs sit on one baseline. */
    float baseline_dev = roundf((y + L.baseline) * scale);

    /* Whole run batched through flux_canvas_draw_glyph_run (flux
     * ADR-0010): one draw call per TEXT_RUN_BATCH glyphs instead of a
     * draw + push-constant update per glyph. */
    flux_glyph_quad quads[TEXT_RUN_BATCH];
    flux_glyph_run_desc run = FLUX_GLYPH_RUN_DESC_INIT;
    run.atlas = t->atlas;
    run.quads = quads;

    uint32_t n = 0;
    for (int i = 0; i < L.count; i++) {
        const txt_placed_glyph *g = &L.glyphs[i];

        /* Horizontal subpixel positioning: keep the fractional pen, split it
         * into an integer device origin and a phase; the glyph is rasterised
         * with that phase baked in so spacing stays even and crisp without
         * snapping the pen (which is what made gaps jitter). */
        float pen_dev = (x + g->x) * scale;
        float origin = floorf(pen_dev);
        int phase;
        if (g->subpixel) {
            phase = (int)lroundf((pen_dev - origin) * TXT_SUBPIXEL_PHASES);
            if (phase >= TXT_SUBPIXEL_PHASES) {
                phase = 0;
                origin += 1.0f;
            }
        } else {
            /* CJK: integer-snap. One cache entry per glyph regardless of
             * pen fraction, so the working set is not multiplied by the
             * subpixel-phase count. */
            phase = 0;
        }

        glyph_entry *e = txt_glyph_get(t, g->face_id, g->gid, L.rpx, (uint8_t)phase);
        if (!e || e->w <= 0 || e->h <= 0)
            continue;

        float dst_x_dev = origin + (float)e->left;
        float dst_y_dev = baseline_dev - roundf(g->y_off * scale) - (float)e->top;

        quads[n++] = (flux_glyph_quad){
            .sx = dst_x_dev * inv_scale,
            .sy = dst_y_dev * inv_scale,
            .sw = (float)e->w * inv_scale,
            .sh = (float)e->h * inv_scale,
            .ax = e->atlas_x,
            .ay = e->atlas_y,
            .aw = (uint16_t)e->w,
            .ah = (uint16_t)e->h,
            .color = color,
        };
        if (n == TEXT_RUN_BATCH) {
            txt_atlas_flush(t);
            run.quad_count = n;
            flux_canvas_draw_glyph_run(canvas, &run);
            n = 0;
        }
    }
    if (n > 0) {
        txt_atlas_flush(t);
        run.quad_count = n;
        flux_canvas_draw_glyph_run(canvas, &run);
    }
}

/* ------------------------------------------------------------------ */
/*  Caret mapping                                                      */
/* ------------------------------------------------------------------ */

/* Visual logical-px x of the caret immediately before logical byte `byte`,
 * relative to the text origin. Uses each glyph's run direction so the caret
 * lands on the correct edge in RTL/mixed text; for pure LTR this reduces to
 * the summed advance of the preceding glyphs. */
float flux_text_x_for_byte(flux_text *t, const char *utf8, size_t len, size_t byte,
                           const flux_text_style *style) {
    if (!t || !t->has_backend || !utf8 || len == 0)
        return 0.0f;
    float size_px, weight;
    bool italic;
    flux_text_family family;
    style_unpack(t, style, &size_px, &weight, &italic, &family);

    txt_text_layout L;
    if (!txt_text_layout_build(t, utf8, len, size_px, weight, italic, family, t->scale, &L))
        return 0.0f;

    /* A glyph whose cluster == byte: caret sits at its leading edge
     * (LTR: left, RTL: right). */
    for (int i = 0; i < L.count; i++) {
        const txt_placed_glyph *g = &L.glyphs[i];
        if ((size_t)g->cluster == byte)
            return g->rtl ? g->x + g->advance : g->x;
    }

    /* Otherwise (end of text, or inside a multi-byte cluster): trailing edge
     * of the nearest preceding glyph in logical order. */
    const txt_placed_glyph *best = NULL;
    for (int i = 0; i < L.count; i++) {
        const txt_placed_glyph *g = &L.glyphs[i];
        if ((size_t)g->cluster < byte && (!best || g->cluster > best->cluster))
            best = g;
    }
    if (best)
        return best->rtl ? best->x : best->x + best->advance;
    return 0.0f;
}

/* Visual rectangles covering the logical byte range [lo, hi). Glyphs are in
 * visual order with a monotonic pen, so contiguous selected glyphs merge into
 * one rect and a direction flip (an unselected glyph appearing between two
 * selected ones in visual order) naturally splits into separate rects. */
int flux_text_selection_rects(flux_text *t, const char *utf8, size_t len, size_t lo, size_t hi,
                              const flux_text_style *style, flux_text_xrange *out, int max) {
    if (!t || !t->has_backend || !utf8 || len == 0 || lo >= hi || max <= 0)
        return 0;
    float size_px, weight;
    bool italic;
    flux_text_family family;
    style_unpack(t, style, &size_px, &weight, &italic, &family);

    txt_text_layout L;
    if (!txt_text_layout_build(t, utf8, len, size_px, weight, italic, family, t->scale, &L))
        return 0;

    int n = 0;
    bool active = false;
    float x0 = 0.0f, x1 = 0.0f;

    for (int i = 0; i < L.count; i++) {
        const txt_placed_glyph *g = &L.glyphs[i];
        bool sel = (size_t)g->cluster >= lo && (size_t)g->cluster < hi;
        if (sel) {
            if (!active) {
                x0 = g->x;
                active = true;
            }
            x1 = g->x + g->advance;
        } else if (active) {
            if (n < max)
                out[n++] = (flux_text_xrange){x0, x1};
            active = false;
        }
    }
    if (active && n < max)
        out[n++] = (flux_text_xrange){x0, x1};
    return n;
}

/* Caret "stops" are the leading edge of each glyph (BiDi-correct) plus the
 * trailing edge of the logically-last glyph. Visual movement walks these stops
 * ordered by x. */
size_t flux_text_visual_move(flux_text *t, const char *utf8, size_t len, size_t byte, bool forward,
                             const flux_text_style *style) {
    if (!t || !t->has_backend || !utf8 || len == 0)
        return byte;
    float size_px, weight;
    bool italic;
    flux_text_family family;
    style_unpack(t, style, &size_px, &weight, &italic, &family);

    txt_text_layout L;
    if (!txt_text_layout_build(t, utf8, len, size_px, weight, italic, family, t->scale, &L) ||
        L.count == 0)
        return byte;

    /* Current caret x (leading edge of `byte`). */
    float cur_x = flux_text_x_for_byte(t, utf8, len, byte, style);

    /* Trailing stop: end of the logically-last char. */
    const txt_placed_glyph *last = &L.glyphs[0];
    for (int i = 1; i < L.count; i++)
        if (L.glyphs[i].cluster > last->cluster)
            last = &L.glyphs[i];
    size_t end_byte = len;
    float end_x = last->rtl ? last->x : last->x + last->advance;

    /* Pick the neighbouring stop in x on the requested side. */
    size_t best_byte = byte;
    float best_x = forward ? 1e30f : -1e30f;
    bool found = false;

    for (int i = 0; i <= L.count; i++) {
        size_t b;
        float x;
        if (i < L.count) {
            const txt_placed_glyph *g = &L.glyphs[i];
            b = (size_t)g->cluster;
            x = g->rtl ? g->x + g->advance : g->x;
        } else {
            b = end_byte;
            x = end_x;
        }
        if (forward) {
            if (x > cur_x + 0.01f && x < best_x) {
                best_x = x;
                best_byte = b;
                found = true;
            }
        } else {
            if (x < cur_x - 0.01f && x > best_x) {
                best_x = x;
                best_byte = b;
                found = true;
            }
        }
    }
    return found ? best_byte : byte;
}

/* Source byte offset of the caret nearest local x (relative to text origin). */
size_t flux_text_byte_for_x(flux_text *t, const char *utf8, size_t len, float local_x,
                            const flux_text_style *style) {
    if (!t || !t->has_backend || !utf8 || len == 0)
        return 0;
    float size_px, weight;
    bool italic;
    flux_text_family family;
    style_unpack(t, style, &size_px, &weight, &italic, &family);

    txt_text_layout L;
    if (!txt_text_layout_build(t, utf8, len, size_px, weight, italic, family, t->scale, &L))
        return 0;

    float pen = 0.0f;
    for (int i = 0; i < L.count; i++) {
        if (local_x < pen + L.glyphs[i].advance * 0.5f)
            return (size_t)L.glyphs[i].cluster;
        pen += L.glyphs[i].advance;
    }
    return len;
}

/* ------------------------------------------------------------------ */
/*  Memory reclamation                                                 */
/* ------------------------------------------------------------------ */

void flux_text_compact(flux_text *t) {
    /* layout_buf and runs_buf follow a high-water policy: they grow to fit
     * the largest input ever seen and stay that size until destroy. For a
     * one-off megabyte paste followed by ordinary typing, that wastes the
     * peak allocation indefinitely. flux_text_compact releases the scratch
     * buffers back to empty and invalidates the layout cache; the next
     * measure/draw/caret call reallocates to whatever size it actually
     * needs. Cheap to call every frame, intended for the host's idle path. */
    if (!t)
        return;
    free(t->layout_buf);
    t->layout_buf = NULL;
    t->layout_cap = 0;
    free(t->runs_buf);
    t->runs_buf = NULL;
    free(t->run_levels_buf);
    t->run_levels_buf = NULL;
    t->runs_cap = 0;
    for (int i = 0; i < 32; i++) {
        t->layout_cache[i].last_used = 0;
        free(t->layout_cache[i].utf8);
        t->layout_cache[i].utf8 = NULL;
        t->layout_cache[i].cap = 0;
        free(t->layout_cache[i].layout_buf);
        t->layout_cache[i].layout_buf = NULL;
        t->layout_cache[i].layout_cap = 0;
    }
}
