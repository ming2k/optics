/* txt_platform_font_coretext.c — macOS font-discovery backend for
 * flux-text, implementing txt_platform_font.h over CoreText /
 * CoreFoundation (pure C API; no Objective-C / .m file needed).
 *
 * !!! NOT TESTED ON macOS !!!
 * This file was written on Linux against the public CoreText API; no
 * macOS SDK was available to compile or run it. It is written
 * defensively: every Create/Copy result is NULL-checked and every
 * retained CF object is CFRelease'd exactly once. Review before shipping
 * a macOS build.
 *
 * Mapping to the interface:
 *  - Family query: a CTFontDescriptor carrying (family name, traits:
 *    weight + italic) is expanded with
 *    CTFontDescriptorCreateMatchingFontDescriptors, best match first,
 *    capped by the caller's max_results. Unlike fontconfig this does NOT
 *    cross family boundaries; per-codepoint fallback below picks up
 *    CJK/emoji coverage.
 *  - Codepoint query: CTFontCreateForCharacters on the family-matched
 *    base font — CoreText's cascade/fallback mechanism — then a coverage
 *    verification (mirroring the fontconfig backend).
 *  - File path: CTFontDescriptorCopyAttribute(kCTFontURLAttribute) →
 *    CFURLGetFileSystemRepresentation.
 *  - Coverage set: the CTFont plus its cached CFCharacterSet
 *    (CFCharacterSetIsLongCharacterMember handles > BMP scalars).
 *
 * Face index caveat: CoreText does not expose the FreeType face index of
 * a .ttc member, so matches always report index 0. System fonts that are
 * .ttc collections (Helvetica, Menlo, ...) may therefore resolve to the
 * wrong member face (typically the first, e.g. regular) — acceptable for
 * a first port; refine via kCTFontRegistrationScope or PostScript-name
 * matching against FreeType's own enumeration later.
 *
 * Generic family names passed by face.c map to always-present system
 * families ("sans-serif" → Helvetica, etc.).
 */

#include "txt_platform_font.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreText/CoreText.h>

/* ------------------------------------------------------------------ */
/*  Coverage set (txtp_charset wraps a CTFont + cached character set)  */
/* ------------------------------------------------------------------ */

struct txtp_charset {
    CTFontRef font;           /* retained */
    CFCharacterSetRef chars;  /* owned copy; NULL = coverage unknown */
};

bool txtp_charset_has_char(const txtp_charset *cs, uint32_t cp) {
    if (!cs || !cs->chars)
        return false;
    return CFCharacterSetIsLongCharacterMember(cs->chars, (UTF32Char)cp) != false;
}

void txtp_charset_free(txtp_charset *cs) {
    if (!cs)
        return;
    if (cs->chars)
        CFRelease(cs->chars);
    if (cs->font)
        CFRelease(cs->font);
    free(cs);
}

/* Takes its own retain on font. NULL charset on failure is acceptable:
 * face.c treats unknown coverage as "covers nothing". */
static txtp_charset *txtp_charset_from_font(CTFontRef font) {
    if (!font)
        return NULL;
    txtp_charset *cs = (txtp_charset *)calloc(1, sizeof *cs);
    if (!cs)
        return NULL;
    cs->font = (CTFontRef)CFRetain(font);
    cs->chars = CTFontCopyCharacterSet(font); /* may be NULL */
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
/*  Shared helpers                                                     */
/* ------------------------------------------------------------------ */

/* Generic names arrive from face.c; map them to families that ship with
 * every macOS release. A custom family name (FLUX_TEXT_FONT) is used
 * verbatim. */
static const char *map_generic_family(const char *family_name) {
    if (strcmp(family_name, "serif") == 0)
        return "Times";
    if (strcmp(family_name, "monospace") == 0)
        return "Menlo";
    if (strcmp(family_name, "sans-serif") == 0)
        return "Helvetica";
    return family_name;
}

/* CSS 1..1000 → kCTFontWeightTrait (-1.0 .. 1.0, 0.0 = regular).
 * The slope puts CSS 700 at exactly kCTFontWeightBold (0.4). */
static CGFloat css_to_ct_weight(float w) {
    if (w <= 0.0f)
        w = 400.0f;
    if (w < 1.0f)
        w = 1.0f;
    if (w > 1000.0f)
        w = 1000.0f;
    CGFloat v = (CGFloat)(w - 400.0f) / (CGFloat)750.0;
    if (v < (CGFloat)-1.0)
        v = (CGFloat)-1.0;
    if (v > (CGFloat)1.0)
        v = (CGFloat)1.0;
    return v;
}

/* Build a descriptor carrying (family name, weight/italic traits).
 * Caller CFReleases. NULL on failure. */
static CTFontDescriptorRef make_descriptor(const char *family_name, float weight, bool italic) {
    CFStringRef name =
        CFStringCreateWithCString(kCFAllocatorDefault, family_name, kCFStringEncodingUTF8);
    if (!name)
        return NULL;

    CGFloat w = css_to_ct_weight(weight);
    CFNumberRef wnum = CFNumberCreate(kCFAllocatorDefault, kCFNumberCGFloatType, &w);
    uint32_t symbolic = italic ? (uint32_t)kCTFontItalicTrait : 0u;
    CFNumberRef snum = CFNumberCreate(kCFAllocatorDefault, kCFNumberUInt32Type, &symbolic);

    CTFontDescriptorRef desc = NULL;
    CFDictionaryRef traits = NULL;
    if (wnum && snum) {
        CFStringRef tkeys[] = {kCTFontWeightTrait, kCTFontSymbolicTrait};
        CFTypeRef tvals[] = {wnum, snum};
        traits = CFDictionaryCreate(kCFAllocatorDefault, (const void **)tkeys,
                                    (const void **)tvals, 2, &kCFTypeDictionaryKeyCallBacks,
                                    &kCFTypeDictionaryValueCallBacks);
    }
    if (traits) {
        CFStringRef akeys[] = {kCTFontFamilyNameAttribute, kCTFontTraitsAttribute};
        CFTypeRef avals[] = {name, traits};
        CFDictionaryRef attrs =
            CFDictionaryCreate(kCFAllocatorDefault, (const void **)akeys, (const void **)avals, 2,
                               &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        if (attrs) {
            desc = CTFontDescriptorCreateWithAttributes(attrs);
            CFRelease(attrs);
        }
        CFRelease(traits);
    }
    if (snum)
        CFRelease(snum);
    if (wnum)
        CFRelease(wnum);
    CFRelease(name);
    return desc;
}

/* Extract the UTF-8 file path from a font descriptor's URL attribute.
 * Returns a heap string (caller frees) or NULL. */
static char *path_from_descriptor(CTFontDescriptorRef desc) {
    CFURLRef url = (CFURLRef)CTFontDescriptorCopyAttribute(desc, kCTFontURLAttribute);
    if (!url)
        return NULL;
    UInt8 buf[4096];
    Boolean ok = CFURLGetFileSystemRepresentation(url, true, buf, (CFIndex)sizeof(buf));
    CFRelease(url);
    if (!ok)
        return NULL;
    return txtp_strdup((const char *)buf);
}

/* Fill a match from an instantiated font. See the file-header caveat for
 * why index is always 0. */
static bool font_to_match(CTFontRef font, txtp_font_match *out) {
    CTFontDescriptorRef desc = CTFontCopyFontDescriptor(font);
    if (!desc)
        return false;
    out->path = path_from_descriptor(desc);
    CFRelease(desc);
    if (!out->path)
        return false;
    out->index = 0;
    out->charset = txtp_charset_from_font(font);
    return true;
}

/* ------------------------------------------------------------------ */
/*  Family query (ranked primary chain)                                */
/* ------------------------------------------------------------------ */

txtp_font_list *txt_platform_font_query_family(const char *family_name, float weight, bool italic,
                                               int max_results) {
    if (!family_name || max_results <= 0)
        return NULL;

    CTFontDescriptorRef desc = make_descriptor(map_generic_family(family_name), weight, italic);
    if (!desc)
        return NULL;

    /* No mandatory keys: rank all installed fonts by match quality. */
    CFArrayRef matches = CTFontDescriptorCreateMatchingFontDescriptors(desc, NULL);
    CFRelease(desc);
    if (!matches)
        return NULL;

    txtp_font_list *list = (txtp_font_list *)calloc(1, sizeof *list);
    if (!list) {
        CFRelease(matches);
        return NULL;
    }
    list->matches = (txtp_font_match *)calloc((size_t)max_results, sizeof *list->matches);
    if (!list->matches) {
        free(list);
        CFRelease(matches);
        return NULL;
    }

    CFIndex n = CFArrayGetCount(matches);
    for (CFIndex i = 0; i < n && list->count < max_results; i++) {
        CTFontDescriptorRef d =
            (CTFontDescriptorRef)CFArrayGetValueAtIndex(matches, i); /* not owned */
        if (!d)
            continue;
        /* Instantiate so the coverage set and any later fallback anchor
         * on a real font, not just a descriptor. */
        CTFontRef font = CTFontCreateWithFontDescriptor(d, 0.0, NULL);
        if (!font)
            continue;
        txtp_font_match *m = &list->matches[list->count];
        if (font_to_match(font, m))
            list->count++;
        else
            txtp_font_match_clear(m);
        CFRelease(font);
    }

    CFRelease(matches);
    return list;
}

/* ------------------------------------------------------------------ */
/*  Per-codepoint fallback query (charset patch face)                  */
/* ------------------------------------------------------------------ */

bool txt_platform_font_query_codepoint(const char *family_name, float weight, bool italic,
                                       uint32_t cp, txtp_font_match *out) {
    if (!family_name || !out || cp > 0x10FFFFu)
        return false;

    bool ok = false;
    CTFontDescriptorRef desc = NULL;
    CTFontRef base = NULL;
    CTFontRef fb = NULL;

    desc = make_descriptor(map_generic_family(family_name), weight, italic);
    if (!desc)
        goto done;
    base = CTFontCreateWithFontDescriptor(desc, 0.0, NULL);
    if (!base)
        goto done;

    /* UTF-16 encode the scalar (surrogate pair above the BMP). */
    UTF16Char chars[2];
    CFIndex len;
    if (cp > 0xFFFFu) {
        uint32_t v = cp - 0x10000u;
        chars[0] = (UTF16Char)(0xD800u + (v >> 10));
        chars[1] = (UTF16Char)(0xDC00u + (v & 0x3FFu));
        len = 2;
    } else {
        chars[0] = (UTF16Char)cp;
        len = 1;
    }

    /* CoreText cascade: the fallback font that renders these characters. */
    fb = CTFontCreateForCharacters(base, chars, len, NULL);
    if (!fb)
        goto done;

    /* Verify coverage, like the fontconfig backend does — the cascade
     * returns the least-bad candidate, not a guaranteed covering one. */
    {
        CFCharacterSetRef fbchars = CTFontCopyCharacterSet(fb);
        bool covers = fbchars && CFCharacterSetIsLongCharacterMember(fbchars, (UTF32Char)cp);
        if (fbchars)
            CFRelease(fbchars);
        if (!covers)
            goto done;
    }

    ok = font_to_match(fb, out);
    if (!ok)
        txtp_font_match_clear(out);

done:
    if (fb)
        CFRelease(fb);
    if (base)
        CFRelease(base);
    if (desc)
        CFRelease(desc);
    return ok;
}
