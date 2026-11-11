/*
 * svg/svg.h — SVG vector-document compiler: parse once, paint anywhere.
 *
 * Library position: a Layer-0 sibling of `flux`. It owns exactly one domain —
 * turning SVG 1.1 *static vector art* into flux draw primitives — and owns
 * nothing else: no file IO, no font/text, no images, no animation, no
 * scripting, no CSS cascade beyond presentation attributes and `style=""`.
 *
 * Design contract:
 *   - Parse once into an immutable, refcounted `svg_document`; paint it at any
 *     size, any number of times, without reparsing. A document is thread-safe
 *     to *read*; retention is atomic so a document may be shared across
 *     threads and released from any one of them.
 *   - The document is a **flat, document-ordered shape list** with fully
 *     resolved paint. That is what the consumers of this library need: a
 *     cursor or icon sprite is recoloured, mask-baked and recoloured-per-shape
 *     long after parsing, so paint must be a value on the shape, not a scene
 *     graph node.
 *   - Coordinates stay in the document's own user space. `svg_draw` applies the
 *     fit transform; a caller that wants explicit control emits paths with
 *     `svg_path_emit` and drives the canvas transform itself.
 *   - Parsing allocates from libc and the result is device-less by design
 *     (ADR-0136): a document outlives every flux device and is released by
 *     `svg_release`, not by a context.
 *
 * Group opacity: `g opacity="0.5"` multiplies into each descendant shape's
 * effective opacity (see `svg_shape_view::opacity`). This is a deliberate
 * boundary, not an oversight: isolated group compositing needs either a second
 * tree-shaped IR or a saved layer per group, and neither survives the flat
 * per-shape recolour/mask-bake contract above. Art that depends on overlapping
 * children compositing as one group must be pre-flattened. Everything else —
 * element opacity, fill-opacity, stroke-opacity — is exact.
 */

#ifndef SVG_H
#define SVG_H

#include <flux/canvas.h> /* flux_canvas, flux_encoder, flux_path, flux_gradient_stop */
#include <flux/core.h>   /* flux_result */
#include <flux/math.h>   /* flux_color, flux_rect, flux_mat3x2 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <svg/export.h> /* SVG_API — single source of truth */

#ifdef __cplusplus
extern "C" {
#endif

#define SVG_VERSION_MAJOR 0
#define SVG_VERSION_MINOR 0
#define SVG_VERSION_PATCH 78

/* Packed integer version, monotonic — identical layout to FLUX_VERSION_NUMBER. */
#define SVG_VERSION_NUMBER                                                                         \
    (((uint32_t)SVG_VERSION_MAJOR << 16) | ((uint32_t)SVG_VERSION_MINOR << 8) |                    \
     (uint32_t)SVG_VERSION_PATCH)

SVG_API void svg_version(int *major, int *minor, int *patch);
SVG_API uint32_t svg_version_number(void);
SVG_API bool svg_version_check(int major, int minor, int patch);
SVG_API const char *svg_version_string(void);

/* ================================================================== */
/*  Document                                                          */
/* ================================================================== */

/* An immutable parsed document. Refcounted; never mutated after `svg_parse`. */
typedef struct svg_document svg_document;

/* Bounds on what a single document may contain. Every field is a resource
 * ceiling, not a policy knob: an untrusted document must not be able to make
 * the parser allocate without limit. Zero selects the documented default. */
typedef struct svg_parse_limits {
    uint32_t max_shapes;    /* default 4096 */
    uint32_t max_commands;  /* default 65536 */
    uint32_t max_gradients; /* default 256 */
    uint32_t max_elements;  /* default 16384 — every start tag counts */
    uint32_t max_depth;     /* default 32 — nested <g> */
} svg_parse_limits;

typedef struct svg_parse_opts {
    const char *text; /* UTF-8 SVG source; need not be NUL-terminated when
                       * `length` is a real byte count */
    /* Byte length of `text`. Always pass a real count for a `(ptr, len)` pair,
     * including zero: an empty input is well-formed and is refused by the
     * parser, which is diagnosable, instead of being measured past the end of
     * the caller's buffer.
     *
     * Pass `SVG_LENGTH_MEASURE` only when `text` is NUL-terminated and its
     * length is not already known. There is deliberately no "0 means strlen"
     * shorthand: a binding forwarding a caller-supplied length would hit it on
     * empty input, and an out-of-bounds `strlen` is invisible rather than an
     * error. */
    size_t length;
    const svg_parse_limits *limits; /* nullable => defaults */
} svg_parse_opts;

/* The only value of `svg_parse_opts.length` meaning "measure this
 * NUL-terminated buffer". It is the maximum `size_t` so that it can never
 * collide with a real byte count; `SVG__MAX_INPUT_BYTES` rejects it as an
 * input length anyway. */
#define SVG_LENGTH_MEASURE ((size_t)-1)

/* Parse `opts->text` into a document. Fails with FLUX_ERROR_INVALID_ARGUMENT on
 * malformed markup or on a `limits` ceiling; never returns a partial document. */
FLUX_NODISCARD SVG_API flux_result svg_parse(const svg_parse_opts *opts, svg_document **out);

/* Retain/release. `svg_retain` returns the same pointer; `svg_release(NULL)` is
 * a no-op. Refcounting is atomic, so a document may be shared across threads and
 * released from whichever thread holds the last reference. */
SVG_API svg_document *svg_retain(svg_document *doc);
SVG_API void svg_release(svg_document *doc);

/* Live document count for the whole process. This is the drain counter for the
 * `svg` resource kind (INV-ARCH-LEDGER): a document is device-less, so no device
 * ledger can own it, and the only way to prove the release path balances is to
 * read the counter before and after. Counting is unconditional and lock-free. */
SVG_API uint32_t svg_live_documents(void);

/* The document's intrinsic size in user units: the `viewBox` extent when
 * present, else the `width`/`height` attributes, else the geometric bounds of
 * the shapes. Never zero: an empty document reports 1x1 so callers can use the
 * result as an aspect ratio unconditionally. */
SVG_API flux_rect svg_viewport(const svg_document *doc);

/* Value of a root `<svg>` attribute (e.g. "data-hotspot-x"), or NULL. The
 * returned string is owned by the document. SVG has no vocabulary for
 * application metadata, so carriers stamp it here (`data-*`) and this is how it
 * comes back out without the compiler inventing semantics of its own. */
SVG_API const char *svg_attribute(const svg_document *doc, const char *name);

/* ================================================================== */
/*  Flat shape IR                                                     */
/* ================================================================== */

/* The primitive path vocabulary. Anything richer in SVG (arcs, ellipses,
 * rounded rects, smooth reflections) is decomposed into these at parse time, so
 * a consumer never has to re-implement SVG geometry. */
typedef enum svg_path_verb : uint8_t {
    SVG_PATH_MOVE = 0,  /* params: x, y */
    SVG_PATH_LINE = 1,  /* params: x, y */
    SVG_PATH_QUAD = 2,  /* params: cx, cy, x, y */
    SVG_PATH_CUBIC = 3, /* params: c1x, c1y, c2x, c2y, x, y */
    SVG_PATH_CLOSE = 4, /* no params */
} svg_path_verb;

typedef struct svg_path_cmd {
    uint8_t verb; /* svg_path_verb */
    uint8_t _pad[3];
    float params[6]; /* unused slots are zero */
} svg_path_cmd;

typedef enum svg_paint_kind : uint8_t {
    SVG_PAINT_NONE = 0,          /* `none`/`transparent`: this half is not painted */
    SVG_PAINT_CURRENT_COLOR = 1, /* `currentColor`: paint with the caller's theme colour */
    SVG_PAINT_SOLID = 2,         /* a literal colour */
    SVG_PAINT_LINEAR_GRADIENT = 3,
    SVG_PAINT_RADIAL_GRADIENT = 4,
} svg_paint_kind;

typedef struct svg_paint {
    svg_paint_kind kind;
    uint8_t _pad[3];
    /* Straight (non-premultiplied) 0xAARRGGBB for SVG_PAINT_SOLID; zero
     * otherwise. `opacity` is the paint's own alpha multiplier (fill-opacity /
     * stroke-opacity, and the gradient's stop-opacity cascade). */
    flux_color color;
    float opacity;
    /* Gradient index for the gradient kinds, resolved through
     * `svg_gradient_at`. */
    uint32_t gradient;
} svg_paint;

/* A gradient as resolved in document user space: endpoints are already
 * transformed, and `objectBoundingBox` units have been baked against the shape's
 * bounds. Spread is always pad — flux has no tiling gradient (see the ADR). */
typedef struct svg_gradient_view {
    uint32_t stop_count;
    flux_gradient_stop stops[FLUX_GRADIENT_MAX_STOPS]; /* premultiplied */
    flux_point start;                                  /* linear: x1,y1  radial: centre */
    flux_point end;                                    /* linear: x2,y2  radial: unused */
    float radius;                                      /* radial only */
} svg_gradient_view;

/* One painted shape: a single path with its resolved fill and stroke. */
typedef struct svg_shape_view {
    const svg_path_cmd *cmds;
    uint32_t cmd_count;
    svg_paint fill;
    svg_paint stroke;
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
} svg_shape_view;

SVG_API uint32_t svg_shape_count(const svg_document *doc);

/* Fill `out` with shape `index`. Returns FLUX_ERROR_OUT_OF_RANGE for a bad
 * index. The view borrows from the document and is valid while it is alive. */
FLUX_NODISCARD SVG_API flux_result svg_shape_at(const svg_document *doc, uint32_t index,
                                                svg_shape_view *out);

/* Resolve gradient `index` (a `svg_paint::gradient` value). */
FLUX_NODISCARD SVG_API flux_result svg_gradient_at(const svg_document *doc, uint32_t index,
                                                   svg_gradient_view *out);

/* Append a shape's geometry to `path`, in document user space. The caller owns
 * `path` and its arena. */
SVG_API void svg_path_emit(const svg_shape_view *shape, flux_path *path);

/* ================================================================== */
/*  Painting                                                          */
/* ================================================================== */

/* How a document is fitted into the destination box. */
typedef enum svg_fit : uint8_t {
    SVG_FIT_CONTAIN = 0, /* uniform scale, centred, aspect preserved (default) */
    SVG_FIT_STRETCH = 1, /* independent x/y scale onto the box */
} svg_fit;

/* How paints are chosen. */
typedef enum svg_paint_mode : uint8_t {
    /* Each shape keeps its literal paint; `currentColor` resolves to
     * `opts->color`. This is document-faithful painting. */
    SVG_PAINT_THEME = 0,
    /* Every painted shape is drawn in `opts->color`, discarding literal
     * colours but keeping per-shape coverage, opacity and geometry. This is the
     * monochrome mask bake: what an icon tint or a cursor silhouette needs. */
    SVG_PAINT_MONOCHROME = 1,
} svg_paint_mode;

typedef struct svg_draw_opts {
    const svg_document *document;
    /* Exactly one destination. `canvas` records immediately; `encoder`
     * records into an immutable display list. Setting neither, or both, fails
     * with FLUX_ERROR_INVALID_ARGUMENT. */
    flux_canvas *canvas;
    flux_encoder *encoder;
    /* Destination box in the target's current coordinate space. A zero width or
     * height fails with FLUX_ERROR_INVALID_ARGUMENT. */
    flux_rect bounds;
    svg_fit fit;
    svg_paint_mode paint;
    /* Theme colour for SVG_PAINT_CURRENT_COLOR, and the whole paint under
     * SVG_PAINT_MONOCHROME. Ignored (never premultiplied twice) when
     * `opts->opacity` applies on top. */
    flux_color color;
    /* Global alpha multiplier in [0, 1]. Zero means 1.0 (i.e. "unset"), so a
     * caller that wants an invisible draw must skip the call instead. */
    float opacity;
    /* Non-zero replaces every stroke width with this value in destination
     * units — the theme-weight override that keeps an icon set visually
     * interchangeable. Zero keeps the document's own widths. */
    float stroke_width;
} svg_draw_opts;

/* Paint `opts->document` into `opts->canvas` or `opts->encoder`. */
FLUX_NODISCARD SVG_API flux_result svg_draw(const svg_draw_opts *opts);

/* ================================================================== */
/*  Headless rasterization                                            */
/* ================================================================== */

typedef enum svg_pixel_format : uint32_t {
    /* Premultiplied RGBA8, row-major, tightly packed — flux's native CPU canvas
     * layout. */
    SVG_PIXELS_RGBA8_PREMUL = 0,
    /* Premultiplied BGRA8 — the layout DRM/KMS hardware cursor planes and
     * Vulkan `B8G8R8A8_UNORM` sampling expect, produced by channel swap rather
     * than by a second rasterization. */
    SVG_PIXELS_BGRA8_PREMUL = 1,
    /* One 8-bit coverage byte per pixel — the mask-bake readback. */
    SVG_PIXELS_COVERAGE = 2,
} svg_pixel_format;

typedef struct svg_pixmap {
    uint32_t width;
    uint32_t height;
    uint32_t stride; /* bytes per row */
    svg_pixel_format format;
    uint8_t *pixels; /* owned by the pixmap; free with svg_pixmap_deinit */
} svg_pixmap;

typedef struct svg_rasterize_opts {
    const svg_document *document;
    uint32_t width; /* output extent in physical pixels; both must be non-zero */
    uint32_t height;
    /* Samples per axis for the software rasterizer: 0 selects the canvas
     * default (2 -> 4 samples/px), 1 is aliased, up to
     * FLUX_CANVAS_SUPERSAMPLE_MAX. */
    uint32_t supersample;
    svg_pixel_format format;
    svg_paint_mode paint;
    flux_color color;   /* theme / monochrome colour, as in svg_draw */
    float stroke_width; /* as in svg_draw */
} svg_rasterize_opts;

/* Rasterize a document with the headless software canvas. No device, surface or
 * GPU is involved, so this is safe on a worker thread and in CI. */
FLUX_NODISCARD SVG_API flux_result svg_rasterize(const svg_rasterize_opts *opts, svg_pixmap *out);

/* Release `pixmap->pixels` and zero the struct. Safe on an already-empty
 * pixmap. */
SVG_API void svg_pixmap_deinit(svg_pixmap *pixmap);

#ifdef __cplusplus
}
#endif

#endif /* SVG_H */
