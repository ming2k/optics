/*
 * svg/svg.h — SVG vector-document compiler: parse once, paint anywhere.
 *
 * Library position: a Layer-0 sibling of `flux`. It owns exactly one domain —
 * turning SVG 1.1 *static vector art* into flux draw primitives — and owns
 * nothing else: no file IO, no font/text, no images, no animation, no
 * scripting, no CSS cascade beyond presentation attributes and `style=""`.
 *
 * Design contract:
 *   - Parse once into an immutable, refcounted `reticle_document`; paint it at any
 *     size, any number of times, without reparsing. A document is thread-safe
 *     to *read*; retention is atomic so a document may be shared across
 *     threads and released from any one of them.
 *   - The document is a **flat, document-ordered shape list** with fully
 *     resolved paint. That is what the consumers of this library need: a
 *     cursor or icon sprite is recoloured, mask-baked and recoloured-per-shape
 *     long after parsing, so paint must be a value on the shape, not a scene
 *     graph node.
 *   - Coordinates stay in the document's own user space. `reticle_draw` applies the
 *     fit transform; a caller that wants explicit control emits paths with
 *     `reticle_path_emit` and drives the canvas transform itself.
 *   - Parsing allocates from libc and the result is device-less by design
 *     (ADR-0136): a document outlives every flux device and is released by
 *     `reticle_release`, not by a context.
 *
 * Group opacity: `g opacity="0.5"` multiplies into each descendant shape's
 * effective opacity (see `reticle_shape_view::opacity`). This is a deliberate
 * boundary, not an oversight: isolated group compositing needs either a second
 * tree-shaped IR or a saved layer per group, and neither survives the flat
 * per-shape recolour/mask-bake contract above. Art that depends on overlapping
 * children compositing as one group must be pre-flattened. Everything else —
 * element opacity, fill-opacity, stroke-opacity — is exact.
 */

#ifndef RETICLE_H
#define RETICLE_H

#include <flux/canvas.h> /* flux_canvas, flux_encoder, flux_path, flux_gradient_stop */
#include <flux/core.h>   /* flux_result */
#include <flux/math.h>   /* flux_color, flux_rect, flux_mat3x2 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <reticle/export.h> /* RETICLE_API — single source of truth */

#ifdef __cplusplus
extern "C" {
#endif

#define RETICLE_VERSION_MAJOR 0
#define RETICLE_VERSION_MINOR 0
#define RETICLE_VERSION_PATCH 78

/* Packed integer version, monotonic — identical layout to FLUX_VERSION_NUMBER. */
#define RETICLE_VERSION_NUMBER                                                                     \
    (((uint32_t)RETICLE_VERSION_MAJOR << 16) | ((uint32_t)RETICLE_VERSION_MINOR << 8) |            \
     (uint32_t)RETICLE_VERSION_PATCH)

RETICLE_API void reticle_version(int *major, int *minor, int *patch);
RETICLE_API uint32_t reticle_version_number(void);
RETICLE_API bool reticle_version_check(int major, int minor, int patch);
RETICLE_API const char *reticle_version_string(void);

/* ================================================================== */
/*  Document                                                          */
/* ================================================================== */

/* An immutable parsed document. Refcounted; never mutated after `reticle_parse`. */
typedef struct reticle_document reticle_document;

/* Bounds on what a single document may contain. Every field is a resource
 * ceiling, not a policy knob: an untrusted document must not be able to make
 * the parser allocate without limit. Zero selects the documented default. */
typedef struct reticle_parse_limits {
    uint32_t max_shapes;    /* default 4096 */
    uint32_t max_commands;  /* default 65536 */
    uint32_t max_gradients; /* default 256 */
    uint32_t max_elements;  /* default 16384 — every start tag counts */
    uint32_t max_depth;     /* default 32 — nested <g> */
} reticle_parse_limits;

typedef struct reticle_parse_opts {
    const char *text; /* UTF-8 SVG source; need not be NUL-terminated when
                       * `length` is a real byte count */
    /* Byte length of `text`. Always pass a real count for a `(ptr, len)` pair,
     * including zero: an empty input is well-formed and is refused by the
     * parser, which is diagnosable, instead of being measured past the end of
     * the caller's buffer.
     *
     * Pass `RETICLE_LENGTH_MEASURE` only when `text` is NUL-terminated and its
     * length is not already known. There is deliberately no "0 means strlen"
     * shorthand: a binding forwarding a caller-supplied length would hit it on
     * empty input, and an out-of-bounds `strlen` is invisible rather than an
     * error. */
    size_t length;
    const reticle_parse_limits *limits; /* nullable => defaults */
} reticle_parse_opts;

/* The only value of `reticle_parse_opts.length` meaning "measure this
 * NUL-terminated buffer". It is the maximum `size_t` so that it can never
 * collide with a real byte count; `SVG__MAX_INPUT_BYTES` rejects it as an
 * input length anyway. */
#define RETICLE_LENGTH_MEASURE ((size_t)-1)

/* Parse `opts->text` into a document. Fails with FLUX_ERROR_INVALID_ARGUMENT on
 * malformed markup or on a `limits` ceiling; never returns a partial document. */
FLUX_NODISCARD RETICLE_API flux_result reticle_parse(const reticle_parse_opts *opts,
                                                     reticle_document **out);

/* Retain/release. `reticle_retain` returns the same pointer; `reticle_release(NULL)` is
 * a no-op. Refcounting is atomic, so a document may be shared across threads and
 * released from whichever thread holds the last reference. */
RETICLE_API reticle_document *reticle_retain(reticle_document *doc);
RETICLE_API void reticle_release(reticle_document *doc);

/* Live document count for the whole process. This is the drain counter for the
 * `svg` resource kind (INV-ARCH-LEDGER): a document is device-less, so no device
 * ledger can own it, and the only way to prove the release path balances is to
 * read the counter before and after. Counting is unconditional and lock-free. */
RETICLE_API uint32_t reticle_live_documents(void);

/* The document's intrinsic size in user units: the `viewBox` extent when
 * present, else the `width`/`height` attributes, else the geometric bounds of
 * the shapes. Never zero: an empty document reports 1x1 so callers can use the
 * result as an aspect ratio unconditionally. */
RETICLE_API flux_rect reticle_viewport(const reticle_document *doc);

/* Value of a root `<svg>` attribute (e.g. "data-hotspot-x"), or NULL. The
 * returned string is owned by the document. SVG has no vocabulary for
 * application metadata, so carriers stamp it here (`data-*`) and this is how it
 * comes back out without the compiler inventing semantics of its own. */
RETICLE_API const char *reticle_attribute(const reticle_document *doc, const char *name);

/* ================================================================== */
/*  Flat shape IR                                                     */
/* ================================================================== */

/* The primitive path vocabulary. Anything richer in SVG (arcs, ellipses,
 * rounded rects, smooth reflections) is decomposed into these at parse time, so
 * a consumer never has to re-implement SVG geometry. */
typedef enum reticle_path_verb : uint8_t {
    RETICLE_PATH_MOVE = 0,  /* params: x, y */
    RETICLE_PATH_LINE = 1,  /* params: x, y */
    RETICLE_PATH_QUAD = 2,  /* params: cx, cy, x, y */
    RETICLE_PATH_CUBIC = 3, /* params: c1x, c1y, c2x, c2y, x, y */
    RETICLE_PATH_CLOSE = 4, /* no params */
} reticle_path_verb;

typedef struct reticle_path_cmd {
    uint8_t verb; /* reticle_path_verb */
    uint8_t _pad[3];
    float params[6]; /* unused slots are zero */
} reticle_path_cmd;

typedef enum reticle_paint_kind : uint8_t {
    RETICLE_PAINT_NONE = 0,          /* `none`/`transparent`: this half is not painted */
    RETICLE_PAINT_CURRENT_COLOR = 1, /* `currentColor`: paint with the caller's theme colour */
    RETICLE_PAINT_SOLID = 2,         /* a literal colour */
    RETICLE_PAINT_LINEAR_GRADIENT = 3,
    RETICLE_PAINT_RADIAL_GRADIENT = 4,
} reticle_paint_kind;

typedef struct reticle_paint {
    reticle_paint_kind kind;
    uint8_t _pad[3];
    /* Straight (non-premultiplied) 0xAARRGGBB for RETICLE_PAINT_SOLID; zero
     * otherwise. `opacity` is the paint's own alpha multiplier (fill-opacity /
     * stroke-opacity, and the gradient's stop-opacity cascade). */
    flux_color color;
    float opacity;
    /* Gradient index for the gradient kinds, resolved through
     * `reticle_gradient_at`. */
    uint32_t gradient;
} reticle_paint;

/* A gradient as resolved in document user space: endpoints are already
 * transformed, and `objectBoundingBox` units have been baked against the shape's
 * bounds. Spread is always pad — flux has no tiling gradient (see the ADR). */
typedef struct reticle_gradient_view {
    uint32_t stop_count;
    flux_gradient_stop stops[FLUX_GRADIENT_MAX_STOPS]; /* premultiplied */
    flux_point start;                                  /* linear: x1,y1  radial: centre */
    flux_point end;                                    /* linear: x2,y2  radial: unused */
    float radius;                                      /* radial only */
} reticle_gradient_view;

/* One painted shape: a single path with its resolved fill and stroke. */
typedef struct reticle_shape_view {
    const reticle_path_cmd *cmds;
    uint32_t cmd_count;
    reticle_paint fill;
    reticle_paint stroke;
    float stroke_width; /* user units; 0 when the shape is fill-only */
    float miter_limit;
    flux_line_cap cap;
    flux_line_join join;
    flux_fill_rule fill_rule;
    /* Cascaded element opacity: this element's `opacity` times every
     * ancestor `<g>`/`<svg>` opacity. Paint opacity is separate (on the
     * paints) so a consumer that recolours keeps the shape's own alpha. */
    float opacity;
    /* Geometric bounds in user space, stroke excluded. Empty when the shape has
     * no area (a lone horizontal/vertical line). */
    flux_rect bounds;
} reticle_shape_view;

RETICLE_API uint32_t reticle_shape_count(const reticle_document *doc);

/* Fill `out` with shape `index`. Returns FLUX_ERROR_OUT_OF_RANGE for a bad
 * index. The view borrows from the document and is valid while it is alive. */
FLUX_NODISCARD RETICLE_API flux_result reticle_shape_at(const reticle_document *doc, uint32_t index,
                                                        reticle_shape_view *out);

/* Resolve gradient `index` (a `reticle_paint::gradient` value). */
FLUX_NODISCARD RETICLE_API flux_result reticle_gradient_at(const reticle_document *doc,
                                                           uint32_t index,
                                                           reticle_gradient_view *out);

/* Append a shape's geometry to `path`, in document user space. The caller owns
 * `path` and its arena. */
RETICLE_API void reticle_path_emit(const reticle_shape_view *shape, flux_path *path);

/* ================================================================== */
/*  Painting                                                          */
/* ================================================================== */

/* How a document is fitted into the destination box. */
typedef enum reticle_fit : uint8_t {
    RETICLE_FIT_CONTAIN = 0, /* uniform scale, centred, aspect preserved (default) */
    RETICLE_FIT_STRETCH = 1, /* independent x/y scale onto the box */
} reticle_fit;

/* How paints are chosen. */
typedef enum reticle_paint_mode : uint8_t {
    /* Each shape keeps its literal paint; `currentColor` resolves to
     * `opts->color`. This is document-faithful painting. */
    RETICLE_PAINT_THEME = 0,
    /* Every painted shape is drawn in `opts->color`, discarding literal
     * colours but keeping per-shape coverage, opacity and geometry. This is the
     * monochrome mask bake: what an icon tint or a cursor silhouette needs. */
    RETICLE_PAINT_MONOCHROME = 1,
} reticle_paint_mode;

typedef struct reticle_draw_opts {
    const reticle_document *document;
    /* Exactly one destination. `canvas` records immediately; `encoder`
     * records into an immutable display list. Setting neither, or both, fails
     * with FLUX_ERROR_INVALID_ARGUMENT. */
    flux_canvas *canvas;
    flux_encoder *encoder;
    /* Destination box in the target's current coordinate space. A zero width or
     * height fails with FLUX_ERROR_INVALID_ARGUMENT. */
    flux_rect bounds;
    reticle_fit fit;
    reticle_paint_mode paint;
    /* Theme colour for RETICLE_PAINT_CURRENT_COLOR, and the whole paint under
     * RETICLE_PAINT_MONOCHROME. Ignored (never premultiplied twice) when
     * `opts->opacity` applies on top. */
    flux_color color;
    /* Global alpha multiplier in [0, 1]. Zero means 1.0 (i.e. "unset"), so a
     * caller that wants an invisible draw must skip the call instead. */
    float opacity;
    /* Non-zero replaces every stroke width with this value in destination
     * units — the theme-weight override that keeps an icon set visually
     * interchangeable. Zero keeps the document's own widths. */
    float stroke_width;
} reticle_draw_opts;

/* Paint `opts->document` into `opts->canvas` or `opts->encoder`. */
FLUX_NODISCARD RETICLE_API flux_result reticle_draw(const reticle_draw_opts *opts);

/* ================================================================== */
/*  Headless rasterization                                            */
/* ================================================================== */

typedef enum reticle_pixel_format : uint32_t {
    /* Premultiplied RGBA8, row-major, tightly packed — flux's native CPU canvas
     * layout. */
    RETICLE_PIXELS_RGBA8_PREMUL = 0,
    /* Premultiplied BGRA8 — the layout DRM/KMS hardware cursor planes and
     * Vulkan `B8G8R8A8_UNORM` sampling expect, produced by channel swap rather
     * than by a second rasterization. */
    RETICLE_PIXELS_BGRA8_PREMUL = 1,
    /* One 8-bit coverage byte per pixel — the mask-bake readback. */
    RETICLE_PIXELS_COVERAGE = 2,
} reticle_pixel_format;

typedef struct reticle_pixmap {
    uint32_t width;
    uint32_t height;
    uint32_t stride; /* bytes per row */
    reticle_pixel_format format;
    uint8_t *pixels; /* owned by the pixmap; free with reticle_pixmap_deinit */
} reticle_pixmap;

typedef struct reticle_rasterize_opts {
    const reticle_document *document;
    uint32_t width; /* output extent in physical pixels; both must be non-zero */
    uint32_t height;
    /* Samples per axis for the software rasterizer: 0 selects the canvas
     * default (2 -> 4 samples/px), 1 is aliased, up to
     * FLUX_CANVAS_SUPERSAMPLE_MAX. */
    uint32_t supersample;
    reticle_pixel_format format;
    reticle_paint_mode paint;
    flux_color color;   /* theme / monochrome colour, as in reticle_draw */
    float stroke_width; /* as in reticle_draw */
} reticle_rasterize_opts;

/* Rasterize a document with the headless software canvas. No device, surface or
 * GPU is involved, so this is safe on a worker thread and in CI. */
FLUX_NODISCARD RETICLE_API flux_result reticle_rasterize(const reticle_rasterize_opts *opts,
                                                         reticle_pixmap *out);

/* Release `pixmap->pixels` and zero the struct. Safe on an already-empty
 * pixmap. */
RETICLE_API void reticle_pixmap_deinit(reticle_pixmap *pixmap);

#ifdef __cplusplus
}
#endif

#endif /* RETICLE_H */
