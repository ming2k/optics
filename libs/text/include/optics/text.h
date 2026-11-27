/*
 * optics/text.h — text shaping, layout, and glyph-run rendering (ADR-0016, ADR-0106).
 *
 * Sibling library of liboptics_gfx: shapes UTF-8 plus style (size, weight, colour)
 * into positioned glyph quads against a device-uploaded coverage atlas, then
 * batches them through optics_gfx_canvas_draw_glyph_run.
 */

#ifndef OPTICS_TEXT_H
#define OPTICS_TEXT_H

#include <optics/gfx/canvas.h> /* optics_gfx_canvas, optics_gfx_color */
#include <optics/gfx/core.h>   /* optics_gfx_device, optics_gfx_result */
#include <optics/gfx/math.h>   /* optics_gfx_arena */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/*  Visibility                                                        */
/* ================================================================== */

#if defined(_WIN32) && !defined(OPTICS_TEXT_STATIC)
#ifdef OPTICS_TEXT_BUILDING
#define OPTICS_TEXT_API __declspec(dllexport)
#else
#define OPTICS_TEXT_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define OPTICS_TEXT_API __attribute__((visibility("default")))
#else
#define OPTICS_TEXT_API
#endif

#define OPTICS_TEXT_VERSION_MAJOR 0
#define OPTICS_TEXT_VERSION_MINOR 0
#define OPTICS_TEXT_VERSION_PATCH 90

/* Packed integer version, monotonic — identical layout to OPTICS_GFX_VERSION_NUMBER. */
#define OPTICS_TEXT_VERSION_NUMBER                                                                 \
    (((uint32_t)OPTICS_TEXT_VERSION_MAJOR << 16) | ((uint32_t)OPTICS_TEXT_VERSION_MINOR << 8) |    \
     (uint32_t)OPTICS_TEXT_VERSION_PATCH)

OPTICS_TEXT_API void optics_text_version(int *major, int *minor, int *patch);
OPTICS_TEXT_API uint32_t optics_text_version_number(void);
OPTICS_TEXT_API bool optics_text_version_check(int major, int minor, int patch);
OPTICS_TEXT_API const char *optics_text_version_string(void);

/* ================================================================== */
/*  Types                                                             */
/* ================================================================== */

/* Opaque shaping / layout / atlas context. One per device. Thread-affine. */
typedef struct optics_text_ctx optics_text_ctx;

/* Typeface family. */
typedef enum : uint32_t {
    OPTICS_TEXT_FAMILY_DEFAULT = 0, /* use the context default (sans-serif) */
    OPTICS_TEXT_FAMILY_SANS = 1,    /* sans-serif                           */
    OPTICS_TEXT_FAMILY_SERIF = 2,   /* serif                                */
    OPTICS_TEXT_FAMILY_MONO = 3,    /* monospace                            */
} optics_text_family;

/* How a run looks. */
typedef struct optics_text_style {
    float size_px;
    float weight;
    optics_gfx_color color;
    optics_text_family family;
    bool italic;
} optics_text_style;

/* Shaped extent of a run, logical pixels. `baseline` is from the top. */
typedef struct optics_text_metrics {
    float width;
    float height;
    float baseline;
} optics_text_metrics;

/* A horizontal span [x0, x1) in logical pixels. */
typedef struct optics_text_xrange {
    float x0;
    float x1;
} optics_text_xrange;

/* Construction parameters. */
typedef struct optics_text_desc {
    optics_gfx_device *device; /* uploads the glyph atlas; NULL = measure-only */
    float scale;               /* initial device-pixel scale; 0 => 1.0        */
} optics_text_desc;

/* ================================================================== */
/*  Lifecycle                                                         */
/* ================================================================== */

OPTICS_GFX_NODISCARD OPTICS_TEXT_API optics_gfx_result
optics_text_create(const optics_text_desc *desc, optics_text_ctx **out);
OPTICS_GFX_NODISCARD OPTICS_TEXT_API optics_text_ctx *optics_text_retain(optics_text_ctx *t);
OPTICS_TEXT_API void optics_text_release(optics_text_ctx *t);

/* Atlas synchronization contract. */
OPTICS_GFX_NODISCARD OPTICS_TEXT_API bool optics_text_has_pending_uploads(const optics_text_ctx *t);

typedef struct optics_text_flush_atlas_opts {
    optics_gfx_frame *frame; /* nullable: host-atlas paths */
} optics_text_flush_atlas_opts;

OPTICS_GFX_NODISCARD OPTICS_TEXT_API optics_gfx_result
optics_text_flush_atlas(optics_text_ctx *t, const optics_text_flush_atlas_opts *opts);
OPTICS_GFX_NODISCARD OPTICS_TEXT_API uint64_t
optics_text_get_atlas_epoch(const optics_text_ctx *t, const optics_gfx_frame *f);

/* Scale contract. */
OPTICS_TEXT_API void optics_text_set_scale(optics_text_ctx *t, float scale);
OPTICS_TEXT_API float optics_text_scale(const optics_text_ctx *t);

OPTICS_TEXT_API optics_text_family optics_text_default_family(const optics_text_ctx *t);
OPTICS_TEXT_API void optics_text_set_default_family(optics_text_ctx *t, optics_text_family family);

OPTICS_TEXT_API void optics_text_compact(optics_text_ctx *t);

/* ================================================================== */
/*  Diagnostics                                                       */
/* ================================================================== */

typedef struct optics_text_stats {
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
} optics_text_stats;

OPTICS_TEXT_API void optics_text_get_stats(const optics_text_ctx *t, optics_text_stats *out);

/* ================================================================== */
/*  Measure                                                           */
/* ================================================================== */

typedef struct optics_text_measure_opts {
    const char *utf8;
    size_t len;
    const optics_text_style *style; /* nullable: engine defaults */
} optics_text_measure_opts;

OPTICS_TEXT_API optics_text_metrics optics_text_measure(optics_text_ctx *t,
                                                        const optics_text_measure_opts *opts);

/* ================================================================== */
/*  Draw                                                              */
/* ================================================================== */

typedef struct optics_text_draw_opts {
    optics_gfx_canvas *canvas;
    optics_gfx_arena *arena; /* nullable: canvas-owned allocations */
    float x, y;
    const char *utf8;
    size_t len;
    const optics_text_style *style; /* nullable: engine defaults */
    optics_gfx_color outline_color;
    float outline_width; /* 0 = fill only */
} optics_text_draw_opts;

OPTICS_TEXT_API void optics_text_draw(optics_text_ctx *t, const optics_text_draw_opts *opts);

typedef struct optics_text_record_opts {
    optics_gfx_encoder *encoder;
    float x, y;
    const char *utf8;
    size_t len;
    optics_text_style style;
    optics_gfx_color outline_color;
    float outline_width; /* 0 = fill only */
} optics_text_record_opts;

OPTICS_GFX_NODISCARD OPTICS_TEXT_API optics_gfx_result
optics_text_record(optics_text_ctx *t, const optics_text_record_opts *opts);

/* ================================================================== */
/*  Caret and selection mapping (BiDi-correct)                        */
/* ================================================================== */

typedef struct optics_text_x_for_byte_opts {
    const char *utf8;
    size_t len;
    size_t byte;
    const optics_text_style *style; /* nullable: engine defaults */
} optics_text_x_for_byte_opts;

OPTICS_TEXT_API float optics_text_x_for_byte(optics_text_ctx *t,
                                             const optics_text_x_for_byte_opts *opts);

typedef struct optics_text_byte_for_x_opts {
    const char *utf8;
    size_t len;
    float local_x;
    const optics_text_style *style; /* nullable: engine defaults */
} optics_text_byte_for_x_opts;

OPTICS_TEXT_API size_t optics_text_byte_for_x(optics_text_ctx *t,
                                              const optics_text_byte_for_x_opts *opts);

typedef struct optics_text_selection_rects_opts {
    const char *utf8;
    size_t len;
    size_t lo, hi;
    const optics_text_style *style; /* nullable: engine defaults */
    optics_text_xrange *out;
    int max;
} optics_text_selection_rects_opts;

OPTICS_TEXT_API int optics_text_selection_rects(optics_text_ctx *t,
                                                const optics_text_selection_rects_opts *opts);

typedef struct optics_text_visual_move_opts {
    const char *utf8;
    size_t len;
    size_t byte;
    bool forward;
    const optics_text_style *style; /* nullable: engine defaults */
} optics_text_visual_move_opts;

OPTICS_TEXT_API size_t optics_text_visual_move(optics_text_ctx *t,
                                               const optics_text_visual_move_opts *opts);

#ifdef __cplusplus
}
#endif

#endif /* OPTICS_TEXT_H */
