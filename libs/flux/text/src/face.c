/* face.c — font face loading (FreeType + HarfBuzz), slot management and
 * backend lifecycle for the text path. Font discovery itself (family →
 * file path + face index, per-codepoint fallback) lives behind the
 * platform layer in txt_platform_font.h (fontconfig on Linux, DirectWrite
 * on Windows, CoreText on macOS).
 *
 * Two weight slots (regular <=500, bold >500); each holds up to
 * MAX_FACES_PER_SLOT faces from the platform's ranked family query so
 * missing glyphs fall back through the chain. See text_internal.h for
 * the shared state. */

#include "text_internal.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Diagnostics                                                        */
/* ------------------------------------------------------------------ */

void txt_logf(flux_text *t, flux_log_level level, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (t && t->dev) {
        flux_device_log(t->dev, level, "flux-text", "%s", buf);
    } else {
        fprintf(stderr, "[flux-text] %s\n", buf);
    }
}

/* ------------------------------------------------------------------ */
/*  Face loading                                                       */
/* ------------------------------------------------------------------ */

/* Forward decl: txt_load_patch_face (above) and txt_init_slot_faces
 * (below) both need the family→generic-name map. */
static const char *family_fc(flux_text_family fam);
static int txt_load_patch_face(flux_text *t, int slot_idx, uint32_t cp);

static bool face_load(flux_text *t, int slot_idx, int face_idx, uint32_t req_px) {
    txt_face *f = &t->slots[slot_idx].faces[face_idx];
    if (f->valid) {
        if (f->face_px != req_px)
            txt_face_set_px(f, req_px);
        return true;
    }
    if (!f->path)
        return false;

    if (FT_New_Face(t->ft, f->path, f->index, &f->face) != 0) {
        txt_logf(t, FLUX_LOG_ERROR, "FT_New_Face failed for %s", f->path);
        return false;
    }
    if (FT_Set_Pixel_Sizes(f->face, 0, req_px) != 0) {
        FT_Done_Face(f->face);
        f->face = NULL;
        return false;
    }
    f->face_px = req_px;
    f->units_per_em = (float)f->face->units_per_EM;
    if (f->units_per_em <= 0.0f)
        f->units_per_em = 1000.0f;
    f->ascent_em = (float)f->face->ascender / f->units_per_em;
    f->descent_em = (float)(-f->face->descender) / f->units_per_em;

    f->hb_font = hb_ft_font_create_referenced(f->face);
    if (!f->hb_font) {
        FT_Done_Face(f->face);
        f->face = NULL;
        return false;
    }
    /* The initial size enters the retained-size LRU as its front entry so
     * ownership is uniform: the LRU slots own every hb_font/FT_Size,
     * f->hb_font aliases the front slot (see txt_face_set_px and the
     * destroy path). The face's own implicit size object backs slot 0
     * here (no FT_New_Size yet); a later miss recycles it normally. */
    f->size_px[0] = req_px;
    f->sizes[0] = NULL; /* the face's implicit size: no explicit handle */
    f->size_fonts[0] = f->hb_font;
    /* hb-ft sets scale/ppem from the face's current size; keep its default so
     * advances track the pixel size (see txt_face_set_px). */
    f->valid = true;
    return true;
}

bool txt_ensure_face_loaded(flux_text *t, int slot_idx, int face_idx, uint32_t req_px) {
    if (slot_idx < 0 || slot_idx >= FLUX_TEXT_NUM_FAMILIES * TXT_STYLE_SLOTS)
        return false;
    txt_face_slot *slot = &t->slots[slot_idx];
    if (face_idx < 0 || face_idx >= slot->count)
        return false;

    txt_face *f = &slot->faces[face_idx];
    if (f->valid) {
        if (f->face_px != req_px)
            txt_face_set_px(f, req_px);
        return true;
    }
    if (!face_load(t, slot_idx, face_idx, req_px)) {
        free(f->path);
        f->path = NULL;
        if (f->charset) {
            txtp_charset_free(f->charset);
            f->charset = NULL;
        }
        return false;
    }
    return true;
}

int txt_find_face_for_char(flux_text *t, int slot_idx, uint32_t cp) {
    txt_face_slot *slot = &t->slots[slot_idx];
    for (int i = 0; i < slot->count; i++) {
        const txt_face *f = &slot->faces[i];
        if (f->charset && txtp_charset_has_char(f->charset, cp))
            return i;
    }
    /* No loaded face (primary chain or existing patch) covers this
     * codepoint. Ask the platform layer for a covering font and lazily
     * load the top candidate as a patch face. This is what makes rare
     * coverage — CJK Extension-B/C/D/E, historic scripts, emoji —
     * render even when their fonts rank outside the top-8 family query
     * that fills the primary chain. Bounded by MAX_TOTAL_FACES; once
     * full, further uncovered codepoints fall through to face 0
     * (.notdef → tofu) rather than growing without limit. */
    int patch = txt_load_patch_face(t, slot_idx, cp);
    if (patch >= 0)
        return patch;
    return 0;
}

/* Lazily load a charset-targeted patch face for cp. Asks the platform
 * layer for the best installed font covering cp (biased towards the
 * slot's family/weight/italic) and appends it to slot->faces. Returns
 * the new face index, or -1 on failure / cap hit / no installed
 * coverage. */
static int txt_load_patch_face(flux_text *t, int slot_idx, uint32_t cp) {
    txt_face_slot *slot = &t->slots[slot_idx];
    if (slot->count >= MAX_TOTAL_FACES)
        return -1;

    /* Deliberately the plain generic family name, NOT the FLUX_TEXT_FONT
     * override the primary chain honoured — patch queries stay on the
     * generic family so an env override cannot skew rare-codepoint
     * fallback (same semantics as before the platform split). */
    txtp_font_match m = {0};
    if (!txt_platform_font_query_codepoint(family_fc(slot->family), slot->weight, slot->italic, cp,
                                           &m))
        return -1;

    /* Grow the heap array (doubling). The primary chain pre-allocated
     * MAX_FACES_PER_SLOT entries at init; patch loading starts from
     * slot->count. */
    if (slot->count >= slot->cap) {
        int newcap = slot->cap ? slot->cap * 2 : MAX_FACES_PER_SLOT;
        if (newcap > MAX_TOTAL_FACES)
            newcap = MAX_TOTAL_FACES;
        txt_face *nf = realloc(slot->faces, (size_t)newcap * sizeof *nf);
        if (!nf) {
            txtp_font_match_clear(&m);
            return -1;
        }
        memset(nf + slot->cap, 0, (size_t)(newcap - slot->cap) * sizeof *nf);
        slot->faces = nf;
        slot->cap = newcap;
    }
    if (slot->count >= slot->cap) { /* still at the hard cap */
        txtp_font_match_clear(&m);
        return -1;
    }

    txt_face *f = &slot->faces[slot->count];
    memset(f, 0, sizeof *f);
    f->path = txt_strdup(m.path);
    f->index = m.index;
    f->charset = m.charset; /* steal; the interface has no charset copy op */
    m.charset = NULL;
    int new_idx = slot->count;
    txtp_font_match_clear(&m);

    /* Eagerly load the patch at the same baseline pixel size as the
     * primary chain; per-use size changes are applied by
     * txt_ensure_face_loaded → txt_face_set_px. */
    if (!face_load(t, slot_idx, new_idx, 16)) {
        txt_logf(t, FLUX_LOG_WARN,
                 "charset patch face (%s) for U+%04X failed to load; "
                 "glyph will render as .notdef",
                 f->path ? f->path : "<unknown>", (unsigned)cp);
        free(f->path);
        f->path = NULL;
        if (f->charset) {
            txtp_charset_free(f->charset);
            f->charset = NULL;
        }
        memset(f, 0, sizeof *f);
        return -1;
    }

    slot->count++;

    txt_logf(t, FLUX_LOG_INFO, "loaded charset patch face #%d (%s) for U+%04X; slot has %d face(s)",
             new_idx, f->path, (unsigned)cp, slot->count);
    return new_idx;
}

/* ------------------------------------------------------------------ */
/*  Slot initialisation via the platform family query                  */
/* ------------------------------------------------------------------ */

/* Map a resolved family to the generic family name to query. The
 * platform backends map these generic names to platform defaults
 * (fontconfig: native generic aliases; DirectWrite / CoreText: explicit
 * system fonts). */
static const char *family_fc(flux_text_family fam) {
    switch (fam) {
    case FLUX_TEXT_FAMILY_SERIF:
        return "serif";
    case FLUX_TEXT_FAMILY_MONO:
        return "monospace";
    case FLUX_TEXT_FAMILY_SANS:
    default:
        return "sans-serif";
    }
}

bool txt_init_slot_faces(flux_text *t, int slot_idx, float weight, bool italic,
                         flux_text_family family) {
    /* The FLUX_TEXT_FONT env var overrides the default (sans-serif)
     * family only, so a custom font never silently replaces serif/mono. */
    const char *fam = family_fc(family);
    if (family == FLUX_TEXT_FAMILY_SANS) {
        const char *env = getenv("FLUX_TEXT_FONT");
        if (env && *env)
            fam = env;
    }

    txtp_font_list *list = txt_platform_font_query_family(fam, weight, italic, MAX_FACES_PER_SLOT);
    if (!list)
        return false;

    txt_face_slot *slot = &t->slots[slot_idx];
    /* Free any prior heap array (re-init path) and pre-allocate the
     * primary chain. Patch faces grow this array later via
     * txt_load_patch_face, up to MAX_TOTAL_FACES. */
    free(slot->faces);
    slot->faces = calloc(MAX_FACES_PER_SLOT, sizeof *slot->faces);
    if (!slot->faces) {
        txtp_font_list_free(list);
        return false;
    }
    slot->cap = MAX_FACES_PER_SLOT;
    slot->count = 0;
    slot->family = family;
    slot->weight = weight;
    slot->italic = italic;

    for (int i = 0; i < list->count && slot->count < MAX_FACES_PER_SLOT; i++) {
        txtp_font_match *m = &list->matches[i];
        if (!m->path)
            continue;

        txt_face *f = &slot->faces[slot->count];
        f->path = txt_strdup(m->path);

        /* The platform face index carries the chosen TTC member and, for
         * variable fonts under fontconfig, the requested named instance in
         * the high bits (Regular = 0x40000, Bold = 0x70000). Passing 0 to
         * FT_New_Face would load the default instance, so a bold run would
         * render at regular — forward the index instead. */
        f->index = m->index;

        f->charset = m->charset; /* steal; the interface has no charset copy op */
        m->charset = NULL;

        slot->count++;
    }

    txtp_font_list_free(list);

    if (slot->count == 0)
        return false;

    /* Eagerly load every fallback face, not just the primary.
     *
     * FT_New_Face on a large CJK font (Noto Sans CJK ~20 MB) takes
     * 50-200 ms; with up to MAX_FACES_PER_SLOT fallbacks in the slot,
     * the lazy-load path made the first render of any CJK glyph stall
     * for ~1 s inside flux_text_draw. That stall ran in the panel
     * flush stage and tripped the daemon's 3 s watchdog on first
     * indicator banner render.
     *
     * Loading every face at init pays the cost once, before the
     * watchdog is armed (typio's App::init runs with no watchdog
     * constructed). Subsequent renders only pay the FT_Set_Pixel_Sizes
     * delta, which is sub-millisecond. A failed face load is logged
     * and skipped — the primary face is required, fallbacks are best
     * effort. */
    for (int i = 0; i < slot->count; i++) {
        if (!face_load(t, slot_idx, i, 16)) {
            txt_logf(t, FLUX_LOG_WARN,
                     "fallback face #%d (%s) failed to load; "
                     "glyphs missing from this face will fall through to the next",
                     i, slot->faces[i].path ? slot->faces[i].path : "<unknown>");
            if (i == 0)
                return false; /* primary face is mandatory */
        }
    }

    return true;
}

/* ------------------------------------------------------------------ */
/*  Init / shutdown                                                    */
/* ------------------------------------------------------------------ */

flux_text *txt_engine_init(flux_device *dev) {
    flux_text *t = calloc(1, sizeof *t);
    if (!t) {
        txt_logf(t, FLUX_LOG_ERROR, "out of memory");
        return NULL;
    }
    /* Retained, matching the canvas discipline: the atlas image and the
     * nearest sampler are released at shutdown and must never see a dead
     * device. flux_device_retain is NULL-safe. */
    t->dev = flux_device_retain(dev);

    if (FT_Init_FreeType(&t->ft) != 0) {
        txt_logf(t, FLUX_LOG_ERROR, "FT_Init_FreeType failed");
        goto fail;
    }

    /* Slot 0 is the "regular" sans-serif face. The weight argument is on
     * the CSS 1–1000 scale the platform backends interpret (400 = regular,
     * 500 = medium, 700 = bold), NOT fontconfig's own FC_WEIGHT_* constants.
     * Passing FC_WEIGHT_REGULAR (= 80) here used to make fontconfig match
     * the THIN face (80 < 150), so every glyph looked starved — especially
     * CJK, which has more strokes to fill the same em box. */
    t->default_family = FLUX_TEXT_FAMILY_SANS;
    if (!txt_init_slot_faces(t, 0, 400.0f, false, FLUX_TEXT_FAMILY_SANS)) {
        txt_logf(t, FLUX_LOG_ERROR, "no regular font available");
        goto fail;
    }

    t->hb_buf = hb_buffer_create();
    if (!t->hb_buf) {
        txt_logf(t, FLUX_LOG_ERROR, "hb_buffer_create failed");
        goto fail;
    }

    /* Glyph cache: bounded open-addressing table with LRU eviction at
     * GLYPH_HASH_CAP. Sized to start small (grows on demand up to the
     * cap); eviction only fires when the working set genuinely exceeds
     * the cap, which is rare even for long CJK sessions. */
    t->cache = glyph_cache_new(GLYPH_HASH_INIT, GLYPH_HASH_CAP);
    if (!t->cache) {
        txt_logf(t, FLUX_LOG_ERROR, "glyph cache alloc failed");
        goto fail;
    }

    {
        flux_sampler_desc sd = FLUX_SAMPLER_DESC_INIT;
        sd.mag_filter = FLUX_FILTER_NEAREST;
        sd.min_filter = FLUX_FILTER_NEAREST;
        sd.mipmap_mode = FLUX_FILTER_NEAREST;
        sd.address_u = FLUX_ADDRESS_CLAMP_TO_EDGE;
        sd.address_v = FLUX_ADDRESS_CLAMP_TO_EDGE;
        sd.address_w = FLUX_ADDRESS_CLAMP_TO_EDGE;
        if (flux_sampler_create(t->dev, &sd, &t->nearest_sampler) != FLUX_OK)
            t->nearest_sampler = NULL;
    }

    if (!txt_atlas_init(t)) {
        txt_logf(t, FLUX_LOG_ERROR, "atlas init failed");
        goto fail;
    }

    {
        txt_face *f = &t->slots[0].faces[0];
        FT_UInt gi = FT_Get_Char_Index(f->face, 'A');
        if (gi && FT_Load_Glyph(f->face, gi, FT_LOAD_DEFAULT) == 0 &&
            FT_Render_Glyph(f->face->glyph, FT_RENDER_MODE_NORMAL) == 0) {
            FT_Bitmap *b = &f->face->glyph->bitmap;
            txt_logf(t, FLUX_LOG_INFO,
                     "%s (upem=%d, packed R8 atlas %dx%d, glyph 'A' = %dx%d, %d fallback face(s))",
                     f->path, (int)f->units_per_em, ATLAS_W, ATLAS_H, (int)b->width, (int)b->rows,
                     t->slots[0].count - 1);
        } else {
            txt_logf(t, FLUX_LOG_WARN,
                     "%s (upem=%d, packed R8 atlas %dx%d, WARNING: glyph 'A' render failed, %d "
                     "fallback face(s))",
                     f->path, (int)f->units_per_em, ATLAS_W, ATLAS_H, t->slots[0].count - 1);
        }
    }
    return t;

fail:
    txt_engine_shutdown(t);
    return NULL;
}

void txt_engine_shutdown(flux_text *t) {
    if (!t)
        return;
    txt_atlas_destroy(t);
    glyph_cache_destroy(t->cache);
    t->cache = NULL;
    txt_layout_cache_reset(t);
    free(t->layout_buf);
    free(t->atlas_upload_scratch);
    free(t->runs_buf);
    free(t->run_levels_buf);
    if (t->nearest_sampler)
        flux_sampler_release(t->nearest_sampler);
    if (t->hb_buf)
        hb_buffer_destroy(t->hb_buf);
    for (int s = 0; s < FLUX_TEXT_NUM_FAMILIES * TXT_STYLE_SLOTS; s++) {
        txt_face_slot *slot = &t->slots[s];
        for (int i = 0; i < slot->count; i++) {
            txt_face *f = &slot->faces[i];
            /* Retained size LRU: each slot owns an FT_Size and an hb_font
             * (hb-ft bakes the ppem into the font object, so one hb_font
             * per size). f->hb_font is only an alias for slot 0 and is
             * never destroyed here. */
            for (int k = 0; k < TXT_FACE_SIZE_LRU; k++) {
                if (f->size_fonts[k]) {
                    hb_font_destroy(f->size_fonts[k]);
                    f->size_fonts[k] = NULL;
                }
                if (f->sizes[k]) {
                    /* FT_Done_Size deactivates if current; safe while the
                     * face is still alive. */
                    FT_Done_Size(f->sizes[k]);
                    f->sizes[k] = NULL;
                }
                f->size_px[k] = 0;
            }
            f->hb_font = NULL; /* alias only; ownership was in the slots */
            if (f->face)
                FT_Done_Face(f->face);
            if (f->charset)
                txtp_charset_free(f->charset);
            free(f->path);
        }
        free(slot->faces);
        slot->faces = NULL;
        slot->count = slot->cap = 0;
    }
    if (t->ft)
        FT_Done_FreeType(t->ft);
    /* Released last: the atlas image and sampler releases above dereference
     * the device (retire queue / bindless heap), so it must outlive them. */
    flux_device_release(t->dev);
    free(t);
}

/* ------------------------------------------------------------------ */
/*  Public lifecycle (flux-text/text.h)                              */
/* ------------------------------------------------------------------ */

flux_result flux_text_create(const flux_text_desc *desc, flux_text **out) {
    if (!out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    *out = NULL;

    flux_device *dev = desc ? desc->device : NULL;
    float scale = (desc && desc->scale > 0.0f) ? desc->scale : 1.0f;

    /* Try the real backend; on any failure fall back to a measure-only
     * monospace context so callers never branch on backend availability. */
    flux_text *t = txt_engine_init(dev);
    if (t) {
        t->has_backend = true;
    } else {
        t = calloc(1, sizeof *t);
        if (!t)
            return FLUX_ERROR_OUT_OF_MEMORY;
        t->dev = flux_device_retain(dev);
        t->has_backend = false;
    }
    t->scale = scale;
    *out = t;
    return FLUX_OK;
}

void flux_text_destroy(flux_text *t) {
    txt_engine_shutdown(t);
}

void flux_text_get_stats(const flux_text *t, flux_text_stats *out) {
    if (!out)
        return;
    flux_text_stats zero = {0};
    *out = zero;
    if (!t)
        return;
    if (t->cache) {
        glyph_cache_stats gs;
        glyph_cache_get_stats(t->cache, &gs);
        out->glyph_cap = gs.cap;
        out->glyph_count = gs.count;
        out->glyph_max_cap = gs.max_cap;
        out->glyph_hits = gs.hits;
        out->glyph_misses = gs.misses;
        out->glyph_evictions = gs.evictions;
        out->glyph_invalidations = gs.invalidations;
        out->glyph_grows = gs.grows;
    }
    out->atlas_clears = t->atlas_clears;
    out->atlas_pages = t->atlas_page_count;
}
