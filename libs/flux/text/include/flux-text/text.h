/*
 * flux/text.h — text shaping, layout, and glyph-run rendering.
 *
 * In-tree module of libflux (formerly a separate libflux-text). Shapes
 * UTF-8 plus a style (size, weight, colour) into positioned glyph quads
 * against a device-uploaded coverage atlas, then batches them through
 * flux_canvas_draw_glyph_run. The core canvas does no shaping, kerning,
 * or atlas management — that all lives here, on FreeType + HarfBuzz +
 * Fontconfig + FriBidi (gated by the meson -Dshaping option).
 *
 * API layers
 * ----------
 *   Layer 0 (this header): single-run primitives — shape/measure/draw one
 *     contiguous run of one style. Sufficient for labels and for callers
 *     that do their own line/paragraph composition.
 *
 *   Layer 1 (reserved: `flux_text_layout`): a retained, cached layout
 *     object built from multiple styled runs with line wrapping. Not yet
 *     defined — do not assume its shape.
 *
 * Conventions
 * -----------
 *   - Public symbols are `flux_text_*`; library internals are not
 *     exported.
 *   - Creation: `flux_result flux_text_create(const desc*, out**)`.
 *   - Strings are (pointer, length) UTF-8 — never assumed NUL-terminated,
 *     so callers can pass slices of a larger buffer without copying.
 *   - All public coordinates and sizes are in *logical* pixels. The
 *     device-pixel scale lives on the context (flux_text_set_scale), not
 *     in every call, so glyphs rasterise crisp under the canvas HiDPI
 *     transform.
 *   - The monospace fallback is internal: a context is always usable for
 *     measuring even when no shaping backend is present, so callers never
 *     branch on backend availability.
 */

#ifndef FLUX_TEXT_H
#define FLUX_TEXT_H

#include <flux/canvas.h> /* flux_canvas, flux_color                       */
#include <flux/core.h>   /* flux_device, flux_result                      */
#include <flux/math.h>   /* flux_arena                                    */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/*  Visibility — unified with libflux (FLUX_API from <flux/core.h>).  */
/* ================================================================== */

#define FLUX_TEXT_VERSION_MAJOR 0
#define FLUX_TEXT_VERSION_MINOR 0
#define FLUX_TEXT_VERSION_PATCH 3

FLUX_API const char *flux_text_version_string(void);

/* ================================================================== */
/*  Types                                                             */
/* ================================================================== */

/* Opaque shaping / layout / atlas context. One per device. Thread-affine:
 * do not share an instance across threads without external locking. */
typedef struct flux_text flux_text;

/* Typeface family. The default (0) defers to the context's configured
 * default family, so existing callers that zero-initialise a style keep
 * the historic sans-serif look without setting it explicitly. */
typedef enum {
    FLUX_TEXT_FAMILY_DEFAULT = 0, /* use the context default (sans-serif) */
    FLUX_TEXT_FAMILY_SANS = 1,    /* sans-serif                           */
    FLUX_TEXT_FAMILY_SERIF = 2,   /* serif                                */
    FLUX_TEXT_FAMILY_MONO = 3,    /* monospace                            */
} flux_text_family;

/* How a run looks. Passed by const pointer so future fields (slant,
 * font features, decoration) can be appended without changing any
 * function signature; pass NULL for the context defaults. `weight` 0
 * selects the regular weight. `color` is ignored by measure/caret calls.
 * `family` selects the typeface family; FLUX_TEXT_FAMILY_DEFAULT (the
 * zero value) falls back to the context's default family set through
 * flux_text_set_default_family (itself sans-serif at creation). */
typedef struct flux_text_style {
    float size_px;
    float weight;
    flux_color color;
    flux_text_family family;
    bool italic;
} flux_text_style;

/* Shaped extent of a run, logical pixels. `baseline` is from the top. */
typedef struct flux_text_metrics {
    float width;
    float height;
    float baseline;
} flux_text_metrics;

/* A horizontal span [x0, x1) in logical pixels, used to report the
 * on-screen extent of a selected byte range (one per visual line). */
typedef struct flux_text_xrange {
    float x0;
    float x1;
} flux_text_xrange;

/* Construction parameters. Zero-initialise and set what you need:
 *   (flux_text_desc){ .device = dev } */
typedef struct flux_text_desc {
    flux_device *device; /* uploads the glyph atlas; NULL = measure-only */
    float scale;         /* initial device-pixel scale; 0 => 1.0        */
} flux_text_desc;

/* ================================================================== */
/*  Lifecycle                                                         */
/* ================================================================== */

/* Create a text context. On success `*out` is a usable context — it can
 * measure even when no shaping backend is compiled in (it degrades to
 * monospace metrics internally), so callers never branch on backend
 * availability. Returns an error only on real failure (e.g. allocation).
 * Destroy with flux_text_destroy. */
FLUX_NODISCARD FLUX_API flux_result flux_text_create(const flux_text_desc *desc, flux_text **out);
FLUX_API void flux_text_destroy(flux_text *t);

/* Set the device-pixel scale used to rasterise glyphs (default 1.0).
 * Constant across a frame; set it once when the surface scale changes. */
FLUX_API void flux_text_set_scale(flux_text *t, float scale);
FLUX_API float flux_text_scale(const flux_text *t);

/* Get/set the context's default typeface family — the family a style with
 * FLUX_TEXT_FAMILY_DEFAULT resolves to. Sans-serif at creation. Setting it
 * loads the family's faces lazily on first use. */
FLUX_API flux_text_family flux_text_default_family(const flux_text *t);
FLUX_API void flux_text_set_default_family(flux_text *t, flux_text_family family);

/* Release the per-context scratch high-water marks (the placed-glyph buffer,
 * the run list, and the layout cache). They grow to fit the largest input
 * ever seen and stay that size; calling this after a one-off megabyte paste
 * (or whenever the host goes idle) returns that peak memory to the system.
 * Cheap to call every frame. The next measure/draw/caret call simply
 * reallocates to whatever size it needs. */
FLUX_API void flux_text_compact(flux_text *t);

/* ================================================================== */
/*  Diagnostics                                                       */
/* ================================================================== */

/* Snapshot of the glyph cache + texture atlas. Safe to call any time
 * the caller owns the context. Intended for long-session health
 * monitoring: a climbing eviction count or atlas_clears total signals
 * the cache is churning — the documented cause of "gets laggy after
 * a while" under long CJK sessions. */
typedef struct flux_text_stats {
    uint32_t glyph_cap;           /* current glyph table capacity (pow2) */
    uint32_t glyph_count;         /* live glyph entries                  */
    uint32_t glyph_max_cap;       /* hard ceiling before eviction        */
    uint64_t glyph_hits;          /* lookups that found an entry         */
    uint64_t glyph_misses;        /* lookups that did not                */
    uint64_t glyph_evictions;     /* entries dropped by put() at cap     */
    uint64_t glyph_invalidations; /* entries dropped by clear()          */
    uint64_t glyph_grows;         /* table doublings                     */
    uint64_t atlas_clears;        /* full-atlas reclaims (texture full)  */
} flux_text_stats;

FLUX_API void flux_text_get_stats(const flux_text *t, flux_text_stats *out);

/* ================================================================== */
/*  Measure                                                          */
/* ================================================================== */

/* Shape `len` bytes of `utf8` in `style` and report the extent. Returns a
 * zeroed metric for empty input. `style` NULL uses context defaults. */
FLUX_API flux_text_metrics flux_text_measure(flux_text *t, const char *utf8, size_t len,
                                             const flux_text_style *style);

/* ================================================================== */
/*  Draw                                                             */
/* ================================================================== */

/* Shape `len` bytes of `utf8` and paint them as a single batched glyph
 * run with the top-left at (x, y) in logical pixels, using `style`
 * (including its colour). No-op for a measure-only context. */
FLUX_API void flux_text_draw(flux_text *t, flux_canvas *canvas, flux_arena *arena, float x, float y,
                             const char *utf8, size_t len, const flux_text_style *style);

/* ================================================================== */
/*  Caret and selection mapping (BiDi-correct)                       */
/* ================================================================== */

/* Logical x of the glyph boundary before byte `byte`. Under BiDi the
 * prefix width is not the caret x, so use this rather than measuring a
 * substring. */
FLUX_API float flux_text_x_for_byte(flux_text *t, const char *utf8, size_t len, size_t byte,
                                    const flux_text_style *style);

/* Source byte offset of the glyph boundary nearest logical x `local_x`. */
FLUX_API size_t flux_text_byte_for_x(flux_text *t, const char *utf8, size_t len, float local_x,
                                     const flux_text_style *style);

/* Fill `out` (capacity `max`) with the on-screen spans covering byte
 * range [lo, hi). Returns the number written. */
FLUX_API int flux_text_selection_rects(flux_text *t, const char *utf8, size_t len, size_t lo,
                                       size_t hi, const flux_text_style *style,
                                       flux_text_xrange *out, int max);

/* Move the caret one glyph in visual order (forward = rightward on
 * screen) and return the resulting source byte offset. */
FLUX_API size_t flux_text_visual_move(flux_text *t, const char *utf8, size_t len, size_t byte,
                                      bool forward, const flux_text_style *style);

#ifdef __cplusplus
}
#endif

#endif /* FLUX_TEXT_H */
