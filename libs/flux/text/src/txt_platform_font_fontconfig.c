/* txt_platform_font_fontconfig.c — Linux font-discovery backend for
 * flux-text, implementing txt_platform_font.h over fontconfig.
 *
 * This is the original face.c fontconfig code, moved verbatim behind the
 * platform interface: FcFontSort for the ranked primary chain, a
 * charset-constrained FcFontSort for per-codepoint patch faces, and
 * FcCharSet as the coverage set. Behaviour on Linux is meant to be
 * byte-for-byte identical to the pre-split code, including the hardcoded
 * /usr/share/fonts fallback list at the bottom (kept here deliberately —
 * it is a Linux-ism and must not leak into the interface layer). */

#include "txt_platform_font.h"

#include <fontconfig/fontconfig.h>

#include <stdio.h>

/* ------------------------------------------------------------------ */
/*  Coverage set (txtp_charset wraps FcCharSet)                        */
/* ------------------------------------------------------------------ */

struct txtp_charset {
    FcCharSet *fc; /* owned */
};

bool txtp_charset_has_char(const txtp_charset *cs, uint32_t cp) {
    if (!cs || !cs->fc)
        return false;
    return FcCharSetHasChar(cs->fc, (FcChar32)cp) == FcTrue;
}

void txtp_charset_free(txtp_charset *cs) {
    if (!cs)
        return;
    if (cs->fc)
        FcCharSetDestroy(cs->fc);
    free(cs);
}

static txtp_charset *txtp_charset_from_fc(const FcCharSet *src) {
    if (!src)
        return NULL;
    txtp_charset *cs = calloc(1, sizeof *cs);
    if (!cs)
        return NULL;
    cs->fc = FcCharSetCopy((FcCharSet *)src);
    if (!cs->fc) {
        free(cs);
        return NULL;
    }
    return cs;
}

/* ------------------------------------------------------------------ */
/*  Match / list lifecycle                                             */
/* ------------------------------------------------------------------ */

void txtp_font_match_clear(txtp_font_match *m) {
    if (!m)
        return;
    free(m->path);
    m->path = NULL;
    txtp_charset_free(m->charset);
    m->charset = NULL;
    m->index = 0;
}

void txtp_font_list_free(txtp_font_list *list) {
    if (!list)
        return;
    for (int i = 0; i < list->count; i++)
        txtp_font_match_clear(&list->matches[i]);
    free(list->matches);
    free(list);
}

/* ------------------------------------------------------------------ */
/*  Weight mapping (verbatim from the original face.c)                 */
/* ------------------------------------------------------------------ */

static int weight_to_fc(float weight) {
    if (weight <= 0.0f)
        weight = 400.0f;
    if (weight < 150.0f)
        return FC_WEIGHT_THIN;
    if (weight < 250.0f)
        return FC_WEIGHT_EXTRALIGHT;
    if (weight < 350.0f)
        return FC_WEIGHT_LIGHT;
    if (weight < 450.0f)
        return FC_WEIGHT_REGULAR;
    if (weight < 550.0f)
        return FC_WEIGHT_MEDIUM;
    if (weight < 650.0f)
        return FC_WEIGHT_SEMIBOLD;
    if (weight < 750.0f)
        return FC_WEIGHT_BOLD;
    if (weight < 850.0f)
        return FC_WEIGHT_EXTRABOLD;
    return FC_WEIGHT_BLACK;
}

/* ------------------------------------------------------------------ */
/*  Family query (ranked primary chain)                                */
/* ------------------------------------------------------------------ */

/* Last-resort font paths, tried (with an fopen existence probe) only when
 * the fontconfig sort produced zero usable files. Linux-specific by
 * design; other platforms have their own always-present system fonts. */
static const char *const hardcoded[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    NULL,
};

txtp_font_list *txt_platform_font_query_family(const char *family_name, float weight, bool italic,
                                               int max_results) {
    if (!family_name || max_results <= 0)
        return NULL;
    if (!FcInit())
        return NULL;

    int fc_slant = italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN;
    FcPattern *pat = FcPatternBuild(NULL, FC_FAMILY, FcTypeString, (const FcChar8 *)family_name,
                                    FC_WEIGHT, FcTypeInteger, weight_to_fc(weight), FC_SLANT,
                                    FcTypeInteger, fc_slant, (char *)NULL);
    if (!pat)
        return NULL;
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult res = FcResultNoMatch;
    FcFontSet *fs = FcFontSort(NULL, pat, FcTrue, NULL, &res);
    FcPatternDestroy(pat);
    if (!fs)
        return NULL;

    txtp_font_list *list = calloc(1, sizeof *list);
    if (!list) {
        FcFontSetDestroy(fs);
        return NULL;
    }
    list->matches = calloc((size_t)max_results, sizeof *list->matches);
    if (!list->matches) {
        free(list);
        FcFontSetDestroy(fs);
        return NULL;
    }

    for (int i = 0; i < fs->nfont && list->count < max_results; i++) {
        FcPattern *p = fs->fonts[i];
        FcChar8 *file = NULL;
        if (FcPatternGetString(p, FC_FILE, 0, &file) != FcResultMatch)
            continue;

        txtp_font_match *m = &list->matches[list->count];
        m->path = txtp_strdup((const char *)file);

        /* Fontconfig's face index carries the chosen weight: for variable
         * fonts the requested named instance lives in the high bits (Regular
         * = 0x40000, Bold = 0x70000). Passing 0 to FT_New_Face would load the
         * default instance, so a bold run would render at regular — read and
         * forward the index instead. */
        int idx = 0;
        if (FcPatternGetInteger(p, FC_INDEX, 0, &idx) == FcResultMatch)
            m->index = idx;

        FcCharSet *cs = NULL;
        if (FcPatternGetCharSet(p, FC_CHARSET, 0, &cs) == FcResultMatch && cs)
            m->charset = txtp_charset_from_fc(cs);

        list->count++;
    }

    FcFontSetDestroy(fs);

    if (list->count == 0) {
        for (int i = 0; hardcoded[i] && list->count < max_results; i++) {
            FILE *fp = fopen(hardcoded[i], "rb");
            if (fp) {
                fclose(fp);
                txtp_font_match *m = &list->matches[list->count++];
                m->path = txtp_strdup(hardcoded[i]);
                m->index = 0;
            }
        }
    }

    return list;
}

/* ------------------------------------------------------------------ */
/*  Per-codepoint fallback query (charset patch face)                  */
/* ------------------------------------------------------------------ */

bool txt_platform_font_query_codepoint(const char *family_name, float weight, bool italic,
                                       uint32_t cp, txtp_font_match *out) {
    if (!family_name || !out)
        return false;
    if (!FcInit())
        return false;

    FcCharSet *req_cs = FcCharSetCreate();
    if (!req_cs)
        return false;
    if (!FcCharSetAddChar(req_cs, (FcChar32)cp)) {
        FcCharSetDestroy(req_cs);
        return false;
    }

    FcPattern *pat = FcPatternBuild(NULL, FC_FAMILY, FcTypeString, (const FcChar8 *)family_name,
                                    FC_WEIGHT, FcTypeInteger, weight_to_fc(weight), FC_SLANT,
                                    FcTypeInteger, italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN,
                                    FC_CHARSET, FcTypeCharSet, req_cs, (char *)NULL);
    FcCharSetDestroy(req_cs);
    if (!pat)
        return false;
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult res = FcResultNoMatch;
    FcFontSet *fs = FcFontSort(NULL, pat, FcTrue, NULL, &res);
    FcPatternDestroy(pat);
    if (!fs)
        return false;

    /* FcFontSort ranks by coverage, but with trim=FcTrue the result set
     * may still include faces that do not actually cover cp; pick the
     * first that does. */
    int chosen = -1;
    for (int i = 0; i < fs->nfont; i++) {
        FcCharSet *cs = NULL;
        if (FcPatternGetCharSet(fs->fonts[i], FC_CHARSET, 0, &cs) == FcResultMatch && cs &&
            FcCharSetHasChar(cs, (FcChar32)cp)) {
            chosen = i;
            break;
        }
    }
    if (chosen < 0) {
        FcFontSetDestroy(fs);
        return false; /* no installed font covers cp; caller renders .notdef */
    }

    FcPattern *p = fs->fonts[chosen];
    FcChar8 *file = NULL;
    if (FcPatternGetString(p, FC_FILE, 0, &file) != FcResultMatch) {
        FcFontSetDestroy(fs);
        return false;
    }
    int idx = 0;
    FcPatternGetInteger(p, FC_INDEX, 0, &idx);
    FcCharSet *cs = NULL;
    FcPatternGetCharSet(p, FC_CHARSET, 0, &cs);

    out->path = txtp_strdup((const char *)file);
    out->index = idx;
    out->charset = cs ? txtp_charset_from_fc(cs) : NULL;
    FcFontSetDestroy(fs);
    return out->path != NULL;
}
