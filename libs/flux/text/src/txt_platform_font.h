/* txt_platform_font.h — platform font-discovery layer for flux-text.
 *
 * This is the ONLY platform-specific seam in the text stack. Rasterisation
 * (FreeType), shaping (HarfBuzz) and BiDi (FriBidi) are shared across all
 * platforms; what differs is "family name + weight + italic (+ optional
 * codepoint) → font file path + face index + coverage set":
 *
 *   Linux   → fontconfig  (txt_platform_font_fontconfig.c)
 *   Windows → DirectWrite (txt_platform_font_directwrite.c)
 *   macOS   → CoreText    (txt_platform_font_coretext.c)
 *
 * Exactly one implementation file is compiled per platform (selected in
 * meson.build); all three implement the functions declared below. The
 * interface deliberately carries every behaviour the original fontconfig
 * code had, so no functionality is lost on any platform:
 *
 *  - Ranked candidate list for a (family, weight, italic) query, best
 *    match first (FcFontSort order on Linux). face.c keeps the top
 *    MAX_FACES_PER_SLOT entries as the slot's primary fallback chain.
 *  - Per-codepoint fallback: "which installed font covers this codepoint,
 *    given the slot's family/weight/italic" — the lazy charset patch-face
 *    mechanism (CJK Extension-B, emoji, historic scripts).
 *  - An opaque per-face coverage set (txtp_charset) so face.c can test
 *    codepoint membership without knowing the platform type.
 *  - A FreeType-compatible face index (TTC member, and on fontconfig the
 *    variable-font named instance encoded in the high bits).
 *
 * Semantics shared by all implementations:
 *
 *  - `family_name` is a UTF-8 string. face.c passes either a generic name
 *    ("sans-serif" / "serif" / "monospace") or a concrete family name
 *    (e.g. from the FLUX_TEXT_FONT env override). Backends must map the
 *    generic names to a sensible platform default family.
 *  - `weight` is on the CSS 1..1000 scale (400 = regular, 700 = bold);
 *    values <= 0 are treated as 400. Each backend converts to its own
 *    weight scale internally.
 *  - All returned strings are UTF-8 absolute file paths suitable for
 *    FT_New_Face.
 *  - Ownership: every output is heap-owned by the caller and released via
 *    txtp_font_match_clear / txtp_font_list_free / txtp_charset_free
 *    (all NULL-safe). Callers may steal individual fields out of a
 *    txtp_font_match by NULLing them before clearing.
 */

#ifndef TXT_PLATFORM_FONT_H
#define TXT_PLATFORM_FONT_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque character-coverage set attached to a resolved candidate. Wraps
 * whatever the platform offers (FcCharSet / IDWriteFont::HasCharacter /
 * CFCharacterSet). May be NULL on a match, meaning "coverage unknown" —
 * face.c treats that as "does not cover", exactly like the original
 * fontconfig code did for patterns without an FC_CHARSET value. */
typedef struct txtp_charset txtp_charset;

/* One resolved font candidate. */
typedef struct txtp_font_match {
    /* Heap-owned UTF-8 absolute file path for FT_New_Face. */
    char *path;
    /* Face index passed straight to FT_New_Face. Selects the TTC member;
     * on fontconfig it additionally carries the variable-font named
     * instance in the high bits (Regular = 0x40000, Bold = 0x70000). */
    long index;
    /* Owned coverage set; NULL = unknown. */
    txtp_charset *charset;
} txtp_font_match;

/* Ranked candidate list, best match first. */
typedef struct txtp_font_list {
    txtp_font_match *matches;
    int count;
} txtp_font_list;

/* Query the ranked candidate list for (family_name, weight, italic),
 * best match first, at most max_results entries. Returns NULL on
 * backend failure (face.c then fails the slot init and flux_text_create
 * degrades to the measure-only monospace context). A non-NULL list with
 * count == 0 means "backend works, nothing installed matches". */
txtp_font_list *txt_platform_font_query_family(const char *family_name, float weight, bool italic,
                                               int max_results);

/* Per-codepoint fallback query: resolve the best installed font that
 * actually covers `cp`, biased towards (family_name, weight, italic).
 * On success fills *out (caller owns the fields; release with
 * txtp_font_match_clear) and returns true. Returns false when no
 * installed font covers the codepoint or on backend failure — the
 * caller then renders .notdef (tofu). */
bool txt_platform_font_query_codepoint(const char *family_name, float weight, bool italic,
                                       uint32_t cp, txtp_font_match *out);

/* Release a match's owned fields (path + charset) and zero it. */
void txtp_font_match_clear(txtp_font_match *m);

/* Release a whole list. Matches whose fields were stolen (NULLed) are
 * skipped, so callers can move fields out before freeing the list. */
void txtp_font_list_free(txtp_font_list *list);

/* Coverage test. NULL cs => false (unknown coverage covers nothing). */
bool txtp_charset_has_char(const txtp_charset *cs, uint32_t cp);

/* Release a coverage set. NULL-safe. */
void txtp_charset_free(txtp_charset *cs);

/* Small shared helper for the backends (face.c has its own txt_strdup).
 * strdup is POSIX, not ISO C, so MSVC does not provide it. */
static inline char *txtp_strdup(const char *s) {
    if (!s)
        return NULL;
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

#ifdef __cplusplus
}
#endif

#endif /* TXT_PLATFORM_FONT_H */
