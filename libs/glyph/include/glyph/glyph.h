/*
 * glyph/glyph.h — text shaping, layout, and glyph-run rendering (ADR-0016, ADR-0106).
 *
 * Sibling library of libflux: shapes UTF-8 plus style (size, weight, colour)
 * into positioned glyph quads against a device-uploaded coverage atlas, then
 * batches them through flux_canvas_draw_glyph_run.
 */

#ifndef GLYPH_H
#define GLYPH_H

#include <flux/canvas.h> /* flux_canvas, flux_color */
#include <flux/core.h>   /* flux_device, flux_result */
#include <flux/math.h>   /* flux_arena */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/*  Visibility                                                        */
/* ================================================================== */

#if defined(_WIN32) && !defined(GLYPH_STATIC)
#ifdef GLYPH_BUILDING
#define GLYPH_API __declspec(dllexport)
#else
#define GLYPH_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define GLYPH_API __attribute__((visibility("default")))
#else
#define GLYPH_API
#endif

#define GLYPH_VERSION_MAJOR 0
#define GLYPH_VERSION_MINOR 0
#define GLYPH_VERSION_PATCH 80

/* Packed integer version, monotonic — identical layout to FLUX_VERSION_NUMBER. */
#define GLYPH_VERSION_NUMBER                                                                       \
    (((uint32_t)GLYPH_VERSION_MAJOR << 16) | ((uint32_t)GLYPH_VERSION_MINOR << 8) |                \
     (uint32_t)GLYPH_VERSION_PATCH)

GLYPH_API void glyph_version(int *major, int *minor, int *patch);
GLYPH_API uint32_t glyph_version_number(void);
GLYPH_API bool glyph_version_check(int major, int minor, int patch);
GLYPH_API const char *glyph_version_string(void);

/* ================================================================== */
/*  Types                                                             */
/* ================================================================== */

/* Opaque shaping / layout / atlas context. One per device. Thread-affine. */
typedef struct glyph_ctx glyph_ctx;

/* Typeface family. */
typedef enum : uint32_t {
    GLYPH_FAMILY_DEFAULT = 0, /* use the context default (sans-serif) */
    GLYPH_FAMILY_SANS = 1,    /* sans-serif                           */
    GLYPH_FAMILY_SERIF = 2,   /* serif                                */
    GLYPH_FAMILY_MONO = 3,    /* monospace                            */
} glyph_family;

/* How a run looks. */
typedef struct glyph_style {
    float size_px;
    float weight;
    flux_color color;
    glyph_family family;
    bool italic;
} glyph_style;

/* Shaped extent of a run, logical pixels. `baseline` is from the top. */
typedef struct glyph_metrics {
    float width;
    float height;
    float baseline;
} glyph_metrics;

/* A horizontal span [x0, x1) in logical pixels. */
typedef struct glyph_xrange {
    float x0;
    float x1;
} glyph_xrange;

/* Construction parameters. */
typedef struct glyph_desc {
    flux_device *device; /* uploads the glyph atlas; NULL = measure-only */
    float scale;         /* initial device-pixel scale; 0 => 1.0        */
} glyph_desc;

/* ================================================================== */
/*  Lifecycle                                                         */
/* ================================================================== */

FLUX_NODISCARD GLYPH_API flux_result glyph_create(const glyph_desc *desc, glyph_ctx **out);
FLUX_NODISCARD GLYPH_API glyph_ctx *glyph_retain(glyph_ctx *t);
GLYPH_API void glyph_release(glyph_ctx *t);

/* Atlas synchronization contract. */
FLUX_NODISCARD GLYPH_API bool glyph_has_pending_uploads(const glyph_ctx *t);

typedef struct glyph_flush_atlas_opts {
    flux_frame *frame; /* nullable: host-atlas paths */
} glyph_flush_atlas_opts;

FLUX_NODISCARD GLYPH_API flux_result glyph_flush_atlas(glyph_ctx *t,
                                                       const glyph_flush_atlas_opts *opts);
FLUX_NODISCARD GLYPH_API uint64_t glyph_get_atlas_epoch(const glyph_ctx *t, const flux_frame *f);

/* Scale contract. */
GLYPH_API void glyph_set_scale(glyph_ctx *t, float scale);
GLYPH_API float glyph_scale(const glyph_ctx *t);

GLYPH_API glyph_family glyph_default_family(const glyph_ctx *t);
GLYPH_API void glyph_set_default_family(glyph_ctx *t, glyph_family family);

GLYPH_API void glyph_compact(glyph_ctx *t);

/* ================================================================== */
/*  Diagnostics                                                       */
/* ================================================================== */

typedef struct glyph_stats {
    uint32_t glyph_cap;
    uint32_t glyph_count;
    uint32_t glyph_max_cap;
    uint64_t glyph_hits;
    uint64_t glyph_misses;
    uint64_t glyph_evictions;
    uint64_t glyph_invalidations;
    uint64_t glyph_grows;
    uint64_t atlas_clears;
    uint32_t atlas_pages;
} glyph_stats;

GLYPH_API void glyph_get_stats(const glyph_ctx *t, glyph_stats *out);

/* ================================================================== */
/*  Measure                                                           */
/* ================================================================== */

typedef struct glyph_measure_opts {
    const char *utf8;
    size_t len;
    const glyph_style *style; /* nullable: engine defaults */
} glyph_measure_opts;

GLYPH_API glyph_metrics glyph_measure(glyph_ctx *t, const glyph_measure_opts *opts);

/* ================================================================== */
/*  Draw                                                              */
/* ================================================================== */

typedef struct glyph_draw_opts {
    flux_canvas *canvas;
    flux_arena *arena; /* nullable: canvas-owned allocations */
    float x, y;
    const char *utf8;
    size_t len;
    const glyph_style *style; /* nullable: engine defaults */
    flux_color outline_color;
    float outline_width; /* 0 = fill only */
} glyph_draw_opts;

GLYPH_API void glyph_draw(glyph_ctx *t, const glyph_draw_opts *opts);

typedef struct glyph_record_opts {
    flux_encoder *encoder;
    float x, y;
    const char *utf8;
    size_t len;
    glyph_style style;
    flux_color outline_color;
    float outline_width; /* 0 = fill only */
} glyph_record_opts;

FLUX_NODISCARD GLYPH_API flux_result glyph_record(glyph_ctx *t, const glyph_record_opts *opts);

/* ================================================================== */
/*  Caret and selection mapping (BiDi-correct)                        */
/* ================================================================== */

typedef struct glyph_x_for_byte_opts {
    const char *utf8;
    size_t len;
    size_t byte;
    const glyph_style *style; /* nullable: engine defaults */
} glyph_x_for_byte_opts;

GLYPH_API float glyph_x_for_byte(glyph_ctx *t, const glyph_x_for_byte_opts *opts);

typedef struct glyph_byte_for_x_opts {
    const char *utf8;
    size_t len;
    float local_x;
    const glyph_style *style; /* nullable: engine defaults */
} glyph_byte_for_x_opts;

GLYPH_API size_t glyph_byte_for_x(glyph_ctx *t, const glyph_byte_for_x_opts *opts);

typedef struct glyph_selection_rects_opts {
    const char *utf8;
    size_t len;
    size_t lo, hi;
    const glyph_style *style; /* nullable: engine defaults */
    glyph_xrange *out;
    int max;
} glyph_selection_rects_opts;

GLYPH_API int glyph_selection_rects(glyph_ctx *t, const glyph_selection_rects_opts *opts);

typedef struct glyph_visual_move_opts {
    const char *utf8;
    size_t len;
    size_t byte;
    bool forward;
    const glyph_style *style; /* nullable: engine defaults */
} glyph_visual_move_opts;

GLYPH_API size_t glyph_visual_move(glyph_ctx *t, const glyph_visual_move_opts *opts);

#ifdef __cplusplus
}
#endif

#endif /* GLYPH_H */
