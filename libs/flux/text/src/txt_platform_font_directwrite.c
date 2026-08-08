/* txt_platform_font_directwrite.c — Windows font-discovery backend for
 * flux-text, implementing txt_platform_font.h over DirectWrite (C COM).
 *
 * Compile-verified with `zig cc` against real MinGW-w64 headers
 * (tools/zig-win32-check.sh), so every lpVtbl call site matches the true
 * SDK vtable signatures. NOT runtime-tested on a Windows machine: the
 * logic is written defensively — every HRESULT is checked, every COM
 * pointer is NULL-guarded, and every AddRef is paired with a Release.
 * Review the FIXME-adjacent comments before shipping a Windows build.
 *
 * Mapping to the interface:
 *  - Family query: IDWriteFontCollection::FindFamilyName →
 *    IDWriteFontFamily::GetMatchingFonts(weight, normal stretch, style),
 *    in list order (best match first), capped by the caller's max_results.
 *    Unlike fontconfig this does NOT cross family boundaries, so the
 *    primary chain only covers scripts the family itself covers; the
 *    per-codepoint patch query below is what picks up CJK/emoji fallback.
 *  - Codepoint query: IDWriteFactory2::GetSystemFontFallback →
 *    IDWriteFontFallback::MapCharacters (Windows 8.1+; unavailable on
 *    earlier systems, where the query simply fails and the glyph renders
 *    as .notdef). MapCharacters requires an IDWriteTextAnalysisSource,
 *    implemented inline as a tiny COM object.
 *  - File path: IDWriteFont → IDWriteFontFace::GetFiles →
 *    IDWriteFontFile::{GetLoader,GetReferenceKey} →
 *    IDWriteLocalFontFileLoader::GetFilePathFromKey. Fonts without a
 *    local file (remote/in-memory) are skipped — FreeType needs a path.
 *  - Coverage set: the IDWriteFont itself (HasCharacter), AddRef'd.
 *
 * Generic family names passed by face.c map to always-present system
 * families ("sans-serif" → Segoe UI → Arial, etc.).
 *
 * DirectWrite does not require CoInitialize().
 */

#include "txt_platform_font.h"

#include <dwrite.h>
#include <dwrite_2.h>
#include <windows.h>

/* ------------------------------------------------------------------ */
/*  IID helper: MSVC has __uuidof, everything else uses the IID_ symbols */
/* ------------------------------------------------------------------ */

#ifdef _MSC_VER
#define TXT_IID_OF(x) __uuidof(x)
#else
#define TXT_IID_OF(x) (&IID_##x)
#endif

/* ------------------------------------------------------------------ */
/*  UTF-8 ↔ UTF-16 helpers                                             */
/* ------------------------------------------------------------------ */

static WCHAR *utf8_to_wide(const char *s) {
    if (!s)
        return NULL;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0)
        return NULL;
    WCHAR *w = (WCHAR *)malloc((size_t)n * sizeof(WCHAR));
    if (!w)
        return NULL;
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n)) {
        free(w);
        return NULL;
    }
    return w;
}

static char *wide_to_utf8(const WCHAR *w) {
    if (!w)
        return NULL;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0)
        return NULL;
    char *s = (char *)malloc((size_t)n);
    if (!s)
        return NULL;
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL)) {
        free(s);
        return NULL;
    }
    return s;
}

/* ------------------------------------------------------------------ */
/*  Coverage set (txtp_charset wraps an AddRef'd IDWriteFont)          */
/* ------------------------------------------------------------------ */

struct txtp_charset {
    IDWriteFont *font;
};

bool txtp_charset_has_char(const txtp_charset *cs, uint32_t cp) {
    if (!cs || !cs->font)
        return false;
    BOOL exists = FALSE;
    if (FAILED(cs->font->lpVtbl->HasCharacter(cs->font, (UINT32)cp, &exists)))
        return false;
    return exists != FALSE;
}

void txtp_charset_free(txtp_charset *cs) {
    if (!cs)
        return;
    if (cs->font)
        cs->font->lpVtbl->Release(cs->font);
    free(cs);
}

static txtp_charset *txtp_charset_from_font(IDWriteFont *font) {
    if (!font)
        return NULL;
    txtp_charset *cs = (txtp_charset *)calloc(1, sizeof *cs);
    if (!cs)
        return NULL;
    font->lpVtbl->AddRef(font);
    cs->font = font;
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

static DWRITE_FONT_WEIGHT css_to_dw_weight(float w) {
    if (w <= 0.0f)
        w = 400.0f;
    if (w < 1.0f)
        w = 1.0f;
    if (w > 999.0f)
        w = 999.0f;
    return (DWRITE_FONT_WEIGHT)(int)(w + 0.5f);
}

/* Extract (path, face index, coverage) from an IDWriteFont. The font is
 * NOT consumed; the returned charset holds its own AddRef. */
static bool font_to_match(IDWriteFont *font, txtp_font_match *out) {
    IDWriteFontFace *face = NULL;
    IDWriteFontFile **files = NULL;
    IDWriteFontFileLoader *loader = NULL;
    IDWriteLocalFontFileLoader *local = NULL;
    WCHAR *wpath = NULL;
    UINT32 num_files = 0;
    bool ok = false;

    if (FAILED(font->lpVtbl->CreateFontFace(font, &face)) || !face)
        return false;

    out->index = (long)face->lpVtbl->GetIndex(face);

    if (FAILED(face->lpVtbl->GetFiles(face, &num_files, NULL)) || num_files == 0)
        goto done;
    files = (IDWriteFontFile **)calloc(num_files, sizeof *files);
    if (!files)
        goto done;
    if (FAILED(face->lpVtbl->GetFiles(face, &num_files, files)) || !files[0])
        goto done;

    /* A font can be backed by several files; the first carries the face
     * (the multi-file case is effectively unheard of for local fonts). */
    const void *key = NULL;
    UINT32 key_size = 0;
    if (FAILED(files[0]->lpVtbl->GetReferenceKey(files[0], &key, &key_size)) || !key)
        goto done;
    if (FAILED(files[0]->lpVtbl->GetLoader(files[0], &loader)) || !loader)
        goto done;
    /* Only local files have a path FreeType can open. */
    if (FAILED(loader->lpVtbl->QueryInterface(loader, TXT_IID_OF(IDWriteLocalFontFileLoader),
                                              (void **)&local)) ||
        !local)
        goto done;

    UINT32 path_len = 0;
    if (FAILED(local->lpVtbl->GetFilePathLengthFromKey(local, key, key_size, &path_len)) ||
        path_len == 0)
        goto done;
    wpath = (WCHAR *)malloc(((size_t)path_len + 1) * sizeof(WCHAR));
    if (!wpath)
        goto done;
    if (FAILED(local->lpVtbl->GetFilePathFromKey(local, key, key_size, wpath, path_len + 1)))
        goto done;

    out->path = wide_to_utf8(wpath);
    if (!out->path)
        goto done;
    /* NOTE: FreeType on Windows opens paths with plain fopen (ANSI). A
     * font living under a non-ASCII directory would fail to load; system
     * fonts under C:\Windows\Fonts are pure ASCII in practice. */
    out->charset = txtp_charset_from_font(font); /* NULL on OOM: coverage unknown */
    ok = true;

done:
    if (!ok) { /* never hand back a half-filled match; callers don't clear on false */
        free(out->path);
        out->path = NULL;
    }
    free(wpath);
    if (local)
        local->lpVtbl->Release(local);
    if (loader)
        loader->lpVtbl->Release(loader);
    if (files) {
        for (UINT32 i = 0; i < num_files; i++)
            if (files[i])
                files[i]->lpVtbl->Release(files[i]);
        free(files);
    }
    face->lpVtbl->Release(face);
    return ok;
}

static IDWriteFactory *create_factory(void) {
    IDWriteFactory *factory = NULL;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, TXT_IID_OF(IDWriteFactory),
                                   (IUnknown **)&factory)))
        return NULL;
    return factory;
}

/* Generic names arrive from face.c; map them to families that exist on
 * every supported Windows release, most preferred first. A custom family
 * name (FLUX_TEXT_FONT) is tried verbatim first. */
static const WCHAR *const sans_fallbacks[] = {L"Segoe UI", L"Arial", NULL};
static const WCHAR *const serif_fallbacks[] = {L"Georgia", L"Times New Roman", NULL};
static const WCHAR *const mono_fallbacks[] = {L"Consolas", L"Courier New", NULL};

static const WCHAR *const *generic_fallbacks(const char *family_name) {
    if (strcmp(family_name, "serif") == 0)
        return serif_fallbacks;
    if (strcmp(family_name, "monospace") == 0)
        return mono_fallbacks;
    if (strcmp(family_name, "sans-serif") == 0)
        return sans_fallbacks;
    return NULL; /* custom family: tried verbatim */
}

/* ------------------------------------------------------------------ */
/*  Family query (ranked primary chain)                                */
/* ------------------------------------------------------------------ */

txtp_font_list *txt_platform_font_query_family(const char *family_name, float weight, bool italic,
                                               int max_results) {
    if (!family_name || max_results <= 0)
        return NULL;

    IDWriteFactory *factory = create_factory();
    if (!factory)
        return NULL;

    txtp_font_list *list = NULL;
    IDWriteFontCollection *collection = NULL;
    IDWriteFontFamily *family = NULL;
    IDWriteFontList *fonts = NULL;
    WCHAR *custom_wname = NULL;

    if (FAILED(factory->lpVtbl->GetSystemFontCollection(factory, &collection, TRUE)) ||
        !collection)
        goto done;

    /* Resolve the family name to an index in the collection. */
    UINT32 fam_idx = 0;
    BOOL found = FALSE;
    const WCHAR *const *fallbacks = generic_fallbacks(family_name);
    if (fallbacks) {
        for (int i = 0; fallbacks[i] && !found; i++)
            collection->lpVtbl->FindFamilyName(collection, fallbacks[i], &fam_idx, &found);
    } else {
        custom_wname = utf8_to_wide(family_name);
        if (custom_wname)
            collection->lpVtbl->FindFamilyName(collection, custom_wname, &fam_idx, &found);
        /* Unknown custom family: degrade to the sans chain rather than
         * failing the whole text backend. */
        for (int i = 0; sans_fallbacks[i] && !found; i++)
            collection->lpVtbl->FindFamilyName(collection, sans_fallbacks[i], &fam_idx, &found);
    }
    if (!found)
        goto done;

    if (FAILED(collection->lpVtbl->GetFontFamily(collection, fam_idx, &family)) || !family)
        goto done;

    if (FAILED(family->lpVtbl->GetMatchingFonts(
            family, css_to_dw_weight(weight), DWRITE_FONT_STRETCH_NORMAL,
            italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, &fonts)) ||
        !fonts)
        goto done;

    list = (txtp_font_list *)calloc(1, sizeof *list);
    if (!list)
        goto done;
    list->matches = (txtp_font_match *)calloc((size_t)max_results, sizeof *list->matches);
    if (!list->matches) {
        free(list);
        list = NULL;
        goto done;
    }

    UINT32 n = fonts->lpVtbl->GetFontCount(fonts);
    for (UINT32 i = 0; i < n && list->count < max_results; i++) {
        IDWriteFont *font = NULL;
        if (FAILED(fonts->lpVtbl->GetFont(fonts, i, &font)) || !font)
            continue;
        txtp_font_match *m = &list->matches[list->count];
        if (font_to_match(font, m))
            list->count++;
        else
            txtp_font_match_clear(m); /* belt and braces; font_to_match leaves no junk */
        font->lpVtbl->Release(font);
    }

done:
    free(custom_wname);
    if (fonts)
        fonts->lpVtbl->Release(fonts);
    if (family)
        family->lpVtbl->Release(family);
    if (collection)
        collection->lpVtbl->Release(collection);
    factory->lpVtbl->Release(factory);
    return list;
}

/* ------------------------------------------------------------------ */
/*  Minimal IDWriteTextAnalysisSource for MapCharacters                */
/* ------------------------------------------------------------------ */

typedef struct txt_analysis_source {
    IDWriteTextAnalysisSourceVtbl *lpVtbl;
    LONG ref;
    const WCHAR *text;
    UINT32 len;
} txt_analysis_source;

static HRESULT STDMETHODCALLTYPE tas_QueryInterface(IDWriteTextAnalysisSource *self, REFIID riid,
                                                    void **ppv) {
    if (!ppv)
        return E_POINTER;
    if (IsEqualIID(riid, TXT_IID_OF(IUnknown)) ||
        IsEqualIID(riid, TXT_IID_OF(IDWriteTextAnalysisSource))) {
        *ppv = (void *)self;
        self->lpVtbl->AddRef(self);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE tas_AddRef(IDWriteTextAnalysisSource *self) {
    return (ULONG)InterlockedIncrement(&((txt_analysis_source *)self)->ref);
}

static ULONG STDMETHODCALLTYPE tas_Release(IDWriteTextAnalysisSource *self) {
    txt_analysis_source *s = (txt_analysis_source *)self;
    ULONG r = (ULONG)InterlockedDecrement(&s->ref);
    if (r == 0)
        free(s);
    return r;
}

static HRESULT STDMETHODCALLTYPE tas_GetTextAtPosition(IDWriteTextAnalysisSource *self,
                                                       UINT32 pos, const WCHAR **str, UINT32 *len) {
    txt_analysis_source *s = (txt_analysis_source *)self;
    if (pos < s->len) {
        *str = s->text + pos;
        *len = s->len - pos;
    } else {
        *str = NULL;
        *len = 0;
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE tas_GetTextBeforePosition(IDWriteTextAnalysisSource *self,
                                                           UINT32 pos, const WCHAR **str,
                                                           UINT32 *len) {
    txt_analysis_source *s = (txt_analysis_source *)self;
    if (pos > 0 && pos <= s->len) {
        *str = s->text;
        *len = pos;
    } else {
        *str = NULL;
        *len = 0;
    }
    return S_OK;
}

static DWRITE_READING_DIRECTION STDMETHODCALLTYPE
tas_GetParagraphReadingDirection(IDWriteTextAnalysisSource *self) {
    (void)self;
    return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
}

static HRESULT STDMETHODCALLTYPE tas_GetLocaleName(IDWriteTextAnalysisSource *self, UINT32 pos,
                                                   UINT32 *len, const WCHAR **name) {
    (void)self;
    (void)pos;
    *len = 5;
    *name = L"en-us";
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE tas_GetNumberSubstitution(IDWriteTextAnalysisSource *self,
                                                           UINT32 pos, UINT32 *len,
                                                           IDWriteNumberSubstitution **sub) {
    (void)self;
    (void)pos;
    *len = 0;
    *sub = NULL;
    return S_OK;
}

static const IDWriteTextAnalysisSourceVtbl g_tas_vtbl = {
    tas_QueryInterface,  tas_AddRef,           tas_Release,
    tas_GetTextAtPosition,    tas_GetTextBeforePosition, tas_GetParagraphReadingDirection,
    tas_GetLocaleName,   tas_GetNumberSubstitution,
};

/* ------------------------------------------------------------------ */
/*  Per-codepoint fallback query (charset patch face)                  */
/* ------------------------------------------------------------------ */

bool txt_platform_font_query_codepoint(const char *family_name, float weight, bool italic,
                                       uint32_t cp, txtp_font_match *out) {
    if (!family_name || !out || cp > 0x10FFFFu)
        return false;

    IDWriteFactory *factory = create_factory();
    if (!factory)
        return false;

    bool ok = false;
    IDWriteFactory2 *factory2 = NULL;
    IDWriteFontFallback *fallback = NULL;
    IDWriteFont *mapped = NULL;
    WCHAR *wname = NULL;
    txt_analysis_source *src = NULL;

    /* GetSystemFontFallback needs IDWriteFactory2 (Windows 8.1+). */
    if (FAILED(factory->lpVtbl->QueryInterface(factory, TXT_IID_OF(IDWriteFactory2),
                                               (void **)&factory2)) ||
        !factory2)
        goto done;
    if (FAILED(factory2->lpVtbl->GetSystemFontFallback(factory2, &fallback)) || !fallback)
        goto done;

    /* Base family for the fallback mapping: the custom name verbatim, or
     * the first generic fallback. MapCharacters treats it as a bias, not
     * a hard constraint. */
    const WCHAR *base = NULL;
    const WCHAR *const *fallbacks = generic_fallbacks(family_name);
    if (fallbacks) {
        base = fallbacks[0];
    } else {
        wname = utf8_to_wide(family_name);
        base = wname ? wname : sans_fallbacks[0];
    }

    /* UTF-16 encode the scalar (surrogate pair above the BMP). */
    WCHAR text[2];
    UINT32 text_len;
    if (cp > 0xFFFFu) {
        uint32_t v = cp - 0x10000u;
        text[0] = (WCHAR)(0xD800u + (v >> 10));
        text[1] = (WCHAR)(0xDC00u + (v & 0x3FFu));
        text_len = 2;
    } else {
        text[0] = (WCHAR)cp;
        text_len = 1;
    }

    src = (txt_analysis_source *)calloc(1, sizeof *src);
    if (!src)
        goto done;
    src->lpVtbl = (IDWriteTextAnalysisSourceVtbl *)&g_tas_vtbl;
    src->ref = 1;
    src->text = text;
    src->len = text_len;

    FLOAT scale = 0.0f;
    UINT32 mapped_len = 0; /* covered text length — we map a single scalar, so unused */
    if (FAILED(fallback->lpVtbl->MapCharacters(
            fallback, (IDWriteTextAnalysisSource *)src, 0, text_len, NULL, base,
            css_to_dw_weight(weight),
            italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, &mapped_len, &mapped, &scale)) ||
        !mapped)
        goto done;

    /* MapCharacters does not guarantee coverage (it returns the least-bad
     * candidate); verify, like the fontconfig backend does. */
    BOOL exists = FALSE;
    if (FAILED(mapped->lpVtbl->HasCharacter(mapped, (UINT32)cp, &exists)) || !exists)
        goto done;

    ok = font_to_match(mapped, out);

done:
    if (mapped)
        mapped->lpVtbl->Release(mapped);
    if (src)
        src->lpVtbl->Release((IDWriteTextAnalysisSource *)src);
    free(wname);
    if (fallback)
        fallback->lpVtbl->Release(fallback);
    if (factory2)
        factory2->lpVtbl->Release(factory2);
    factory->lpVtbl->Release(factory);
    return ok;
}
