/* text_internal.h — shared state for the FreeType/HarfBuzz/Fontconfig text
 * backend, split across face.c (face discovery/loading + lifecycle), atlas.c
 * (packed R8 coverage atlas + rect allocation + subpixel rasterisation +
 * full-texture reclaim), glyph_cache.c (open-addressing glyph hash table
 * with bounded LRU eviction), itemize.c (BiDi + script + face run
 * itemisation) and layout.c (shaping + subpixel positioning + the public
 * measure/draw/caret entry points).
 *
 * Only compiled when FLUX_TEXT_HAVE_FTHB is defined. */

#ifndef FLUX_TEXT_INTERNAL_H
#define FLUX_TEXT_INTERNAL_H

#include <flux-text/text.h>

#include <flux/vulkan.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#include <hb-ft.h>
#include <hb.h>

#include <fontconfig/fontconfig.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "glyph_cache.h"

/* ------------------------------------------------------------------ */
/*  Diagnostics                                                        */
/*                                                                    */
/*  Route text-module messages through the device logger when the     */
/*  context has one (so a consumer's flux_log_fn receives them via    */
/*  flux_device_log), else fall back to stderr. The logger path       */
/*  carries "flux-text" as the category; the stderr fallback tags     */
/*  the line the same way so measure-only contexts (no device)        */
/*  still produce identifiable output. Callers pass a format with     */
/*  no trailing newline — the helper adds one for stderr and lets     */
/*  the device logger terminate the line.                             */
/* ------------------------------------------------------------------ */
void txt_logf(flux_text *t, flux_log_level level, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

/* ------------------------------------------------------------------ */
/*  Tunables                                                           */
/* ------------------------------------------------------------------ */

/* ---- Glyph cache + atlas capacity (coupled sizing) -----------------
 *
 * The cache and the R8 atlas must be sized together: the cache caps how
 * many (face,gid,rpx,phase) tuples we track, the atlas caps how many
 * pixel rects fit. Whichever is smaller is the real ceiling.
 *
 *   ATLAS 4096x4096 = 16M px. At a typical CJK UI glyph (~45x45 + pad)
 *   that is ~7800 rects; at Latin ~24x24, ~28000.
 *
 *   GLYPH_HASH_CAP is the hash TABLE size. With LOAD_FACTOR_SHIFT 1 (0.5)
 *   in glyph_cache.c the effective live-entry ceiling is CAP/2 = 8192.
 *   That number must cover the working set.
 *
 * A long CJK session touches ~3500 common Han chars. Naively that is
 * 3500 x TXT_SUBPIXEL_PHASES(4) = 14000 > 8192, which permanently
 * saturates the cache (steady-state eviction; each evicted glyph is
 * re-rasterised via FreeType on its next appearance). The fix is NOT to
 * balloon both caps (memory + an ever-moving wall) but to stop
 * multiplying CJK by 4: txt_script_is_cjk() snaps ideographs to a single
 * phase, bringing the set back to ~3500, comfortably under 8192 with the
 * atlas to spare. Latin and other sparse scripts keep full subpixel
 * positioning.
 */
#define ATLAS_W 4096
#define ATLAS_H 4096
#define ATLAS_PAD 1 /* transparent gutter between glyphs */
#define GLYPH_HASH_CAP 16384
#define GLYPH_HASH_INIT 256
#define TXT_STYLE_SLOTS 4
#define FLUX_TEXT_NUM_FAMILIES 3 /* sans, serif, mono (DEFAULT resolves to one) */
#define MAX_FACES_PER_SLOT 8     /* primary chain from FcFontSort family query */
#define MAX_TOTAL_FACES 24       /* hard cap: primary + lazy charset patches */
/* Initial run scratch capacity (stack-free heap buffer in flux_text). The
 * run list grows on demand, so this only sets the high-water line before
 * the first realloc. Picked to cover typical mixed-script paragraphs
 * without any allocation; pathological 1000-run input just grows. */
#define TXT_RUNS_INIT 64
/* Byte-length above which txt_itemize refuses to allocate working buffers.
 * 1 GiB covers any realistic UI text; beyond it the per-codepoint working
 * set (cps + bos + bidi types/levels ≈ 13 B/byte) starts to threaten the
 * process rather than the layout, and callers should chunk the input. */
#define TXT_MAX_INPUT_BYTES (1u << 30)
#define TXT_SUBPIXEL_PHASES 4 /* horizontal subpixel positions */

/* ------------------------------------------------------------------ */
/*  Types                                                              */
/* ------------------------------------------------------------------ */

typedef struct txt_face {
    FT_Face face;
    hb_font_t *hb_font;
    char *path;
    FcCharSet *charset;
    /* Fontconfig face index, passed straight to FT_New_Face. For variable
     * fonts fontconfig encodes the chosen named instance in the high bits
     * (e.g. Regular = 0x40000, Medium = 0x50000); passing 0 here would load
     * the default instance regardless of the weight we asked fontconfig for,
     * so weight changes never reached variable CJK faces. */
    FT_Long index;
    uint32_t face_px;
    float units_per_em;
    float ascent_em;
    float descent_em;
    bool valid;
} txt_face;

typedef struct txt_face_slot {
    /* Primary chain (FcFontSort family query, up to MAX_FACES_PER_SLOT)
     * followed by lazily-loaded charset patch faces, capped at
     * MAX_TOTAL_FACES total. Heap-grown because patch faces are
     * discovered at itemize time when a codepoint is not covered by
     * any primary face — e.g. CJK Extension-B, whose only installed
     * fonts rank outside the top-8 family sort and so would otherwise
     * render as .notdef (tofu). See txt_load_patch_face. */
    txt_face *faces;
    int count;
    int cap;
    flux_text_family family; /* slot's family, for charset patch queries */
    int fc_weight;           /* slot's FC_WEIGHT, for charset patch queries */
    int fc_slant;            /* slot's FC_SLANT, for charset patch queries */
} txt_face_slot;

/* One cached glyph image. Defined in glyph_cache.h — included above.
 * Keyed by (face_id, gid, rpx, phase): the same outline at a different
 * horizontal subpixel phase is a distinct entry so fractional pen
 * positions stay crisp without snapping. */

/* A run of text that is uniform in direction + script + font face, in the
 * order it should be laid out left-to-right (visual order). */
typedef struct text_run {
    const char *text; /* points into the caller's utf8 */
    size_t byte_off;  /* offset of text[0] within the source string */
    size_t len;
    int slot_idx;
    int face_idx;
    bool rtl;
    hb_script_t script;
} text_run;

/* One positioned glyph in logical pixels, produced by layout and consumed by
 * measure / draw / caret. x is the unsnapped pen origin relative to the text
 * start; cluster is the source byte offset for caret mapping.
 *
 * `subpixel` is false for CJK ideographic scripts: their dense strokes gain
 * nothing from horizontal subpixel positioning at UI sizes, but each phase
 * is a distinct cache entry (xTXT_SUBPIXEL_PHASES). Snapping CJK to one
 * phase keeps long sessions under the glyph-cache cap. */
typedef struct txt_placed_glyph {
    int face_id;
    uint32_t gid;
    float x;       /* logical px, fractional, from text origin (visual) */
    float y_off;   /* logical px vertical offset */
    float advance; /* logical px */
    int cluster;   /* byte offset into source utf8 */
    uint8_t phase; /* horizontal subpixel phase 0..PHASES-1 */
    bool rtl;      /* glyph's run direction (for caret edges) */
    bool subpixel; /* false => snap to integer x (CJK: 1 phase not 4) */
} txt_placed_glyph;

typedef struct txt_text_layout {
    txt_placed_glyph *glyphs;
    int count;
    uint32_t rpx; /* device-px raster size */
    float scale;
    float inv_scale;
    float width;    /* logical px */
    float height;   /* logical px */
    float baseline; /* logical px, from top */
} txt_text_layout;

struct flux_text {
    float scale;      /* device-pixel raster scale (flux_text_set_scale) */
    bool has_backend; /* false => degraded to monospace metrics only     */

    flux_device *dev;
    FT_Library ft;
    hb_buffer_t *hb_buf;
    /* [family * TXT_STYLE_SLOTS + style_slot]. The DEFAULT family is
     * resolved to default_family before indexing, so it never appears here. */
    txt_face_slot slots[FLUX_TEXT_NUM_FAMILIES * TXT_STYLE_SLOTS];
    flux_text_family default_family; /* family used by FLUX_TEXT_FAMILY_DEFAULT */

    /* Glyph hash table + LRU eviction policy. The cache owns "which
     * (face, gid, rpx, phase) tuples do we know"; atlas owns "where do
     * their pixels live". The previous inline (cache + cache_cap +
     * cache_count) trio is what made the bookkeeping bug possible:
     * dead entries were invalidated in atlas.c without decrementing
     * the count, so the table reported permanent saturation. */
    glyph_cache *cache;

    flux_image *atlas;
    uint8_t *atlas_pixels;
    uint32_t atlas_cursor_x;
    uint32_t atlas_cursor_y;
    uint32_t atlas_row_height;
    bool atlas_dirty;
    /* Dirty bounding box (exclusive max) accumulated by
     * txt_atlas_mark_dirty() on each cache-miss blit. Flushed by
     * txt_atlas_flush() as a single GPU upload before the next
     * draw_glyph_run — replaces the per-glyph submit-and-wait that
     * caused one GPU pipeline stall per new glyph. */
    uint16_t atlas_dirty_x0, atlas_dirty_y0;
    uint16_t atlas_dirty_x1, atlas_dirty_y1;
    /* Count of atlas_clear() calls (full-texture reclaims). Climbs when
     * the working set exceeds ATLAS_W*ATLAS_H; each clear forces the
     * next frame to re-rasterise every visible glyph. Exposed via
     * flux_text_get_stats for long-session diagnostics. */
    uint64_t atlas_clears;

    flux_sampler *nearest_sampler;

    /* Reused per-call scratch for the placed-glyph list (grows, never per
     * call alloc). Not reentrant — the UI runs single-threaded. */
    txt_placed_glyph *layout_buf;
    int layout_cap;

    /* Reused per-call scratch for the itemised run list + BiDi levels.
     * Grows on demand to fit the largest input ever seen; freed on
     * shutdown or via flux_text_compact. Holds up to INT_MAX runs in
     * principle — TXT_RUNS_INIT is just the first allocation.
     *
     * run_levels_buf is int8_t to avoid pulling <fribidi.h> into this
     * header; FriBidiLevel is `signed char` so the two are binary
     * compatible and itemize.c casts at the FriBidi call seam. */
    text_run *runs_buf;
    int8_t *run_levels_buf;
    int runs_cap;

    /* Layout cache: an LRU cache of recently shaped strings. Avoids
     * rebuilding the glyph list on every frame for static UI elements.
     * The key is a content fingerprint (string content + style + scale). */
    struct {
        uint32_t last_used;
        char *utf8;
        size_t len;
        size_t cap;
        float size_px;
        float weight;
        bool italic;
        flux_text_family family;
        float scale;

        txt_text_layout layout;
        txt_placed_glyph *layout_buf;
        int layout_cap;
    } layout_cache[32];
    uint32_t layout_cache_tick;
};

/* ------------------------------------------------------------------ */
/*  Small shared helpers                                               */
/* ------------------------------------------------------------------ */

/* CJK ideographic scripts. Their glyphs are stroke-dense, so at UI sizes a
 * horizontal subpixel shift is visually negligible — but it still costs a
 * distinct cache entry per phase (xTXT_SUBPIXEL_PHASES). Snapping these to
 * one phase collapses the CJK working set (~3500 chars) back under the
 * cache's effective ceiling. Latin/Cyrillic/Greek etc. keep full subpixel
 * positioning where it actually matters. */
static inline bool txt_script_is_cjk(hb_script_t sc) {
    return sc == HB_SCRIPT_HAN || sc == HB_SCRIPT_HIRAGANA || sc == HB_SCRIPT_KATAKANA ||
           sc == HB_SCRIPT_HANGUL || sc == HB_SCRIPT_BOPOMOFO;
}

static inline int encode_face_id(int slot_idx, int face_idx) {
    return (slot_idx << 8) | face_idx;
}

/* Decode one UTF-8 scalar from a buffer of `remaining` bytes; returns bytes
 * consumed (>=1), 0xFFFD on error. The remaining-length parameter is what
 * makes this safe for raw (ptr,len) callers whose final codepoint may be
 * truncated — without it the multibyte branches would read p[1..3] past the
 * end of the buffer and could segfault at a page boundary. */
static inline int txt_utf8_decode(const char *p, size_t remaining, FcChar32 *out_cp) {
    unsigned char c = (unsigned char)*p;
    if ((c & 0x80) == 0) {
        *out_cp = c;
        return 1;
    }
    if ((c & 0xe0) == 0xc0) {
        if (remaining >= 2 && (p[1] & 0xc0) == 0x80) {
            *out_cp = ((c & 0x1f) << 6) | (p[1] & 0x3f);
            return 2;
        }
    } else if ((c & 0xf0) == 0xe0) {
        if (remaining >= 3 && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80) {
            *out_cp = ((c & 0x0f) << 12) | ((p[1] & 0x3f) << 6) | (p[2] & 0x3f);
            return 3;
        }
    } else if ((c & 0xf8) == 0xf0) {
        if (remaining >= 4 && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80 &&
            (p[3] & 0xc0) == 0x80) {
            *out_cp =
                ((c & 0x07) << 18) | ((p[1] & 0x3f) << 12) | ((p[2] & 0x3f) << 6) | (p[3] & 0x3f);
            return 4;
        }
    }
    *out_cp = 0xFFFD;
    return 1;
}

/* 0=regular, 1=bold, 2=italic, 3=bold-italic */
static inline int txt_style_to_slot(float weight, bool italic) {
    int w = weight > 500.0f ? 1 : 0;
    int i = italic ? 2 : 0;
    return w + i;
}

/* Map a resolved (non-DEFAULT) family to a 0-based family index for slot
 * indexing. Callers must resolve DEFAULT to a concrete family first. */
static inline int txt_family_index(flux_text_family fam) {
    int i = (int)fam - 1; /* SANS=1 -> 0, SERIF=2 -> 1, MONO=3 -> 2 */
    if (i < 0 || i >= FLUX_TEXT_NUM_FAMILIES)
        i = 0; /* SANS fallback */
    return i;
}

/* Combined slot index for a resolved family + weight + italic. */
static inline int txt_family_to_slot(flux_text_family fam, float weight, bool italic) {
    return txt_family_index(fam) * TXT_STYLE_SLOTS + txt_style_to_slot(weight, italic);
}

/* Resolve a (possibly DEFAULT) family against the context's configured
 * default. DEFAULT and any out-of-range value fall back to the default. */
static inline flux_text_family txt_resolve_family(flux_text *t, flux_text_family fam) {
    switch (fam) {
    case FLUX_TEXT_FAMILY_SANS:
    case FLUX_TEXT_FAMILY_SERIF:
    case FLUX_TEXT_FAMILY_MONO:
        return fam;
    default:
        return t ? t->default_family : FLUX_TEXT_FAMILY_SANS;
    }
}

/* Resize a face and keep its HarfBuzz font in sync. hb-ft caches the FT
 * ppem/scale, so without hb_ft_font_changed() shaped advances would stay
 * frozen at the size the hb_font was created with — making text spacing
 * independent of point size. With it, advances come back in 26.6 px at the
 * new size (advance/64 == pixels). */
static inline bool txt_face_set_px(txt_face *f, uint32_t px) {
    if (FT_Set_Pixel_Sizes(f->face, 0, px) != 0)
        return false;
    f->face_px = px;
    if (f->hb_font)
        hb_ft_font_changed(f->hb_font);
    return true;
}

/* ------------------------------------------------------------------ */
/*  Inter-TU API                                                       */
/* ------------------------------------------------------------------ */

/* face.c */
bool txt_init_slot_faces(flux_text *t, int slot_idx, float weight, bool italic,
                         flux_text_family family);
bool txt_ensure_face_loaded(flux_text *t, int slot_idx, int face_idx, uint32_t req_px);
int txt_find_face_for_char(flux_text *t, int slot_idx, FcChar32 cp);

/* atlas.c */
bool txt_atlas_init(flux_text *t);
void txt_atlas_destroy(flux_text *t);
glyph_entry *txt_glyph_get(flux_text *t, int face_id, uint32_t gid, uint32_t rpx, uint8_t phase);
/* Mark a region of the CPU-side atlas pixels as modified (cache-miss
 * blit path). The region is uploaded to the GPU atlas image in one
 * batch by txt_atlas_flush(), avoiding a per-glyph GPU submit. */
void txt_atlas_mark_dirty(flux_text *t, uint16_t x, uint16_t y, uint16_t w, uint16_t h);
/* Upload the dirty bounding box to the GPU atlas image in a single
 * flux_image_update_region call, then clear the dirty box. No-op if
 * nothing is dirty. Called by flux_text_draw before each
 * flux_canvas_draw_glyph_run so quads never sample un-uploaded texels. */
void txt_atlas_flush(flux_text *t);

/* itemize.c — grows t->runs_buf / t->run_levels_buf to fit, fills them in
 * visual (left-to-right) order and returns the run count. Returns 0 on
 * allocation failure or empty input (runs_buf may be untouched). `len`
 * bounds the source string (no NUL assumption). There is no MAX_RUNS cap —
 * the buffers grow with the input. */
int txt_itemize(flux_text *t, int slot_idx, const char *utf8, size_t len);

/* layout.c — builds the placed-glyph list into t->layout_buf. `len` bounds
 * the source string. `family` is resolved to a concrete family by the caller
 * (never DEFAULT). */
bool txt_text_layout_build(flux_text *t, const char *utf8, size_t len, float size_px, float weight,
                           bool italic, flux_text_family family, float scale, txt_text_layout *out);

/* face.c — engine lifecycle. txt_engine_init returns NULL if FreeType/
 * Fontconfig init or font discovery fails; the public flux_text_create then
 * degrades to a measure-only monospace context. txt_engine_shutdown is
 * null-safe and frees `t` itself (works on a zeroed degraded context too). */
flux_text *txt_engine_init(flux_device *dev);
void txt_engine_shutdown(flux_text *t);

/* mono_stub.c — monospace fallback metrics (always available). */
flux_text_metrics txt_text_measure_mono(const char *utf8, size_t len, float size_px, float weight);

#endif /* FLUX_TEXT_INTERNAL_H */
