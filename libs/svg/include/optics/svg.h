/*
 * svg/svg.h — SVG vector-document compiler: parse once, paint anywhere.
 *
 * Library position: a Layer-0 sibling of `gfx`. It owns exactly one domain —
 * turning SVG 1.1 *static vector art* into gfx draw primitives — and owns
 * nothing else: no file IO, no font/text, no images, no animation, no
 * scripting, no CSS cascade beyond presentation attributes and `style=""`.
 *
 * Design contract:
 *   - Parse once into an immutable, refcounted `optics_svg_document`; paint it at any
 *     size, any number of times, without reparsing. A document is thread-safe
 *     to *read*; retention is atomic so a document may be shared across
 *     threads and released from any one of them.
 *   - The document is a **flat, document-ordered shape list** with fully
 *     resolved paint. That is what the consumers of this library need: a
 *     cursor or icon sprite is recoloured, mask-baked and recoloured-per-shape
 *     long after parsing, so paint must be a value on the shape, not a scene
 *     graph node.
 *   - Coordinates stay in the document's own user space. `optics_svg_draw` applies the
 *     fit transform; a caller that wants explicit control emits paths with
 *     `optics_svg_path_emit` and drives the canvas transform itself.
 *   - Parsing allocates from libc and the result is device-less by design
 *     (ADR-0136): a document outlives every gfx device and is released by
 *     `optics_svg_release`, not by a context.
 *
 * Group opacity: `g opacity="0.5"` multiplies into each descendant shape's
 * effective opacity (see `optics_svg_shape_view::opacity`). This is a deliberate
 * boundary, not an oversight: isolated group compositing needs either a second
 * tree-shaped IR or a saved layer per group, and neither survives the flat
 * per-shape recolour/mask-bake contract above. Art that depends on overlapping
 * children compositing as one group must be pre-flattened. Everything else —
 * element opacity, fill-opacity, stroke-opacity — is exact.
 */

#ifndef OPTICS_SVG_H
#define OPTICS_SVG_H

#include <optics/gfx/canvas.h> /* optics_gfx_canvas, optics_gfx_encoder, optics_gfx_path, optics_gfx_gradient_stop */
#include <optics/gfx/core.h> /* optics_gfx_result */
#include <optics/gfx/math.h> /* optics_gfx_color, optics_gfx_rect, optics_gfx_mat3x2 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <optics/svg/export.h> /* OPTICS_SVG_API — single source of truth */

#ifdef __cplusplus
extern "C" {
#endif

#define OPTICS_SVG_VERSION_MAJOR 0
#define OPTICS_SVG_VERSION_MINOR 0
#define OPTICS_SVG_VERSION_PATCH 84

/* Packed integer version, monotonic — identical layout to OPTICS_GFX_VERSION_NUMBER. */
#define OPTICS_SVG_VERSION_NUMBER                                                                  \
    (((uint32_t)OPTICS_SVG_VERSION_MAJOR << 16) | ((uint32_t)OPTICS_SVG_VERSION_MINOR << 8) |      \
     (uint32_t)OPTICS_SVG_VERSION_PATCH)

OPTICS_SVG_API void optics_svg_version(int *major, int *minor, int *patch);
OPTICS_SVG_API uint32_t optics_svg_version_number(void);
OPTICS_SVG_API bool optics_svg_version_check(int major, int minor, int patch);
OPTICS_SVG_API const char *optics_svg_version_string(void);

/* ================================================================== */
/*  Document                                                          */
/* ================================================================== */

/* An immutable parsed document. Refcounted; never mutated after `svg_parse`. */
typedef struct optics_svg_document optics_svg_document;

/* Bounds on what a single document may contain. Every field is a resource
 * ceiling, not a policy knob: an untrusted document must not be able to make
 * the parser allocate without limit. Zero selects the documented default. */
typedef struct optics_svg_parse_limits {
    uint32_t max_shapes;    /* default 4096 */
    uint32_t max_commands;  /* default 65536 */
    uint32_t max_gradients; /* default 256 */
    uint32_t max_elements;  /* default 16384 — every start tag counts */
    uint32_t max_depth;     /* default 32 — nested <g> */
} optics_svg_parse_limits;

typedef struct optics_svg_parse_opts {
    const char *text; /* UTF-8 SVG source; need not be NUL-terminated when
                       * `length` is a real byte count */
    /* Byte length of `text`. Always pass a real count for a `(ptr, len)` pair,
     * including zero: an empty input is well-formed and is refused by the
     * parser, which is diagnosable, instead of being measured past the end of
     * the caller's buffer.
     *
     * Pass `OPTICS_SVG_LENGTH_MEASURE` only when `text` is NUL-terminated and its
     * length is not already known. There is deliberately no "0 means strlen"
     * shorthand: a binding forwarding a caller-supplied length would hit it on
     * empty input, and an out-of-bounds `strlen` is invisible rather than an
     * error. */
    size_t length;
    const optics_svg_parse_limits *limits; /* nullable => defaults */
} optics_svg_parse_opts;

/* The only value of `svg_parse_opts.length` meaning "measure this
 * NUL-terminated buffer". It is the maximum `size_t` so that it can never
 * collide with a real byte count; the parser's hard input ceiling rejects it
 * as an input length anyway. */
#define OPTICS_SVG_LENGTH_MEASURE ((size_t)-1)

/* Parse `opts->text` into a document. Fails with OPTICS_GFX_ERROR_INVALID_ARGUMENT on
 * malformed markup or on a `limits` ceiling; never returns a partial document. */
OPTICS_GFX_NODISCARD OPTICS_SVG_API optics_gfx_result
optics_svg_parse(const optics_svg_parse_opts *opts, optics_svg_document **out);

/* Retain/release. `optics_svg_retain` returns the same pointer; `optics_svg_release(NULL)` is
 * a no-op. Refcounting is atomic, so a document may be shared across threads and
 * released from whichever thread holds the last reference. */
OPTICS_SVG_API optics_svg_document *optics_svg_retain(optics_svg_document *doc);
OPTICS_SVG_API void optics_svg_release(optics_svg_document *doc);

/* Live document count for the whole process. This is the drain counter for the
 * `svg` resource kind (INV-ARCH-LEDGER): a document is device-less, so no device
 * ledger can own it, and the only way to prove the release path balances is to
 * read the counter before and after. Counting is unconditional and lock-free. */
OPTICS_SVG_API uint32_t optics_svg_live_documents(void);

/* The document's intrinsic size in user units: the `viewBox` extent when
 * present, else the `width`/`height` attributes, else the geometric bounds of
 * the shapes. Never zero: an empty document reports 1x1 so callers can use the
 * result as an aspect ratio unconditionally. */
OPTICS_SVG_API optics_gfx_rect optics_svg_viewport(const optics_svg_document *doc);

/* Value of a root `<svg>` attribute (e.g. "data-hotspot-x"), or NULL. The
 * returned string is owned by the document. SVG has no vocabulary for
 * application metadata, so carriers stamp it here (`data-*`) and this is how it
 * comes back out without the compiler inventing semantics of its own. */
OPTICS_SVG_API const char *optics_svg_attribute(const optics_svg_document *doc, const char *name);

/* ================================================================== */
/*  Flat shape IR                                                     */
/* ================================================================== */

/* The primitive path vocabulary. Anything richer in SVG (arcs, ellipses,
 * rounded rects, smooth reflections) is decomposed into these at parse time, so
 * a consumer never has to re-implement SVG geometry. */
typedef enum optics_svg_path_verb : uint8_t {
    OPTICS_SVG_PATH_MOVE = 0,  /* params: x, y */
    OPTICS_SVG_PATH_LINE = 1,  /* params: x, y */
    OPTICS_SVG_PATH_QUAD = 2,  /* params: cx, cy, x, y */
    OPTICS_SVG_PATH_CUBIC = 3, /* params: c1x, c1y, c2x, c2y, x, y */
    OPTICS_SVG_PATH_CLOSE = 4, /* no params */
} optics_svg_path_verb;

typedef struct optics_svg_path_cmd {
    uint8_t verb; /* optics_svg_path_verb */
    uint8_t _pad[3];
    float params[6]; /* unused slots are zero */
} optics_svg_path_cmd;

typedef enum optics_svg_paint_kind : uint8_t {
    OPTICS_SVG_PAINT_NONE = 0,          /* `none`/`transparent`: this half is not painted */
    OPTICS_SVG_PAINT_CURRENT_COLOR = 1, /* `currentColor`: paint with the caller's theme colour */
    OPTICS_SVG_PAINT_SOLID = 2,         /* a literal colour */
    OPTICS_SVG_PAINT_LINEAR_GRADIENT = 3,
    OPTICS_SVG_PAINT_RADIAL_GRADIENT = 4,
} optics_svg_paint_kind;

typedef struct optics_svg_paint {
    optics_svg_paint_kind kind;
    uint8_t _pad[3];
    /* Straight (non-premultiplied) 0xAARRGGBB for OPTICS_SVG_PAINT_SOLID; zero
     * otherwise. `opacity` is the paint's own alpha multiplier (fill-opacity /
     * stroke-opacity, and the gradient's stop-opacity cascade). */
    optics_gfx_color color;
    float opacity;
    /* Gradient index for the gradient kinds, resolved through
     * `optics_svg_gradient_at`. */
    uint32_t gradient;
} optics_svg_paint;

/* A gradient as resolved in document user space: endpoints are already
 * transformed, and `objectBoundingBox` units have been baked against the shape's
 * bounds. Spread is always pad — gfx has no tiling gradient (see the ADR). */
typedef struct optics_svg_gradient_view {
    uint32_t stop_count;
    optics_gfx_gradient_stop stops[OPTICS_GFX_GRADIENT_MAX_STOPS]; /* premultiplied */
    optics_gfx_point start; /* linear: x1,y1  radial: centre */
    optics_gfx_point end;   /* linear: x2,y2  radial: unused */
    float radius;           /* radial only */
} optics_svg_gradient_view;

/* One painted shape: a single path with its resolved fill and stroke. */
typedef struct optics_svg_shape_view {
    const optics_svg_path_cmd *cmds;
    uint32_t cmd_count;
    optics_svg_paint fill;
    optics_svg_paint stroke;
    float stroke_width; /* user units; 0 when the shape is fill-only */
    float miter_limit;
    optics_gfx_line_cap cap;
    optics_gfx_line_join join;
    optics_gfx_fill_rule fill_rule;
    /* Cascaded element opacity: this element's `opacity` times every
     * ancestor `<g>`/`<svg>` opacity. Paint opacity is separate (on the
     * paints) so a consumer that recolours keeps the shape's own alpha. */
    float opacity;
    /* Geometric bounds in user space, stroke excluded. Empty when the shape has
     * no area (a lone horizontal/vertical line). */
    optics_gfx_rect bounds;
} optics_svg_shape_view;

OPTICS_SVG_API uint32_t optics_svg_shape_count(const optics_svg_document *doc);

/* Fill `out` with shape `index`. Returns OPTICS_GFX_ERROR_OUT_OF_RANGE for a bad
 * index. The view borrows from the document and is valid while it is alive. */
OPTICS_GFX_NODISCARD OPTICS_SVG_API optics_gfx_result
optics_svg_shape_at(const optics_svg_document *doc, uint32_t index, optics_svg_shape_view *out);

/* Resolve gradient `index` (a `optics_svg_paint::gradient` value). */
OPTICS_GFX_NODISCARD OPTICS_SVG_API optics_gfx_result optics_svg_gradient_at(
    const optics_svg_document *doc, uint32_t index, optics_svg_gradient_view *out);

/* Append a shape's geometry to `path`, in document user space. The caller owns
 * `path` and its arena. */
OPTICS_SVG_API void optics_svg_path_emit(const optics_svg_shape_view *shape, optics_gfx_path *path);

/* ================================================================== */
/*  Painting                                                          */
/* ================================================================== */

/* How a document is fitted into the destination box. */
typedef enum optics_svg_fit : uint8_t {
    OPTICS_SVG_FIT_CONTAIN = 0, /* uniform scale, centred, aspect preserved (default) */
    OPTICS_SVG_FIT_STRETCH = 1, /* independent x/y scale onto the box */
} optics_svg_fit;

/* How paints are chosen. */
typedef enum optics_svg_paint_mode : uint8_t {
    /* Each shape keeps its literal paint; `currentColor` resolves to
     * `opts->color`. This is document-faithful painting. */
    OPTICS_SVG_PAINT_THEME = 0,
    /* Every painted shape is drawn in `opts->color`, discarding literal
     * colours but keeping per-shape coverage, opacity and geometry. This is the
     * monochrome mask bake: what an icon tint or a cursor silhouette needs. */
    OPTICS_SVG_PAINT_MONOCHROME = 1,
} optics_svg_paint_mode;

typedef struct optics_svg_draw_opts {
    const optics_svg_document *document;
    /* Exactly one destination. `canvas` records immediately; `encoder`
     * records into an immutable display list. Setting neither, or both, fails
     * with OPTICS_GFX_ERROR_INVALID_ARGUMENT. */
    optics_gfx_canvas *canvas;
    optics_gfx_encoder *encoder;
    /* Destination box in the target's current coordinate space. A zero width or
     * height fails with OPTICS_GFX_ERROR_INVALID_ARGUMENT. */
    optics_gfx_rect bounds;
    optics_svg_fit fit;
    optics_svg_paint_mode paint;
    /* Theme colour for OPTICS_SVG_PAINT_CURRENT_COLOR, and the whole paint under
     * OPTICS_SVG_PAINT_MONOCHROME. Ignored (never premultiplied twice) when
     * `opts->opacity` applies on top. */
    optics_gfx_color color;
    /* Global alpha multiplier in [0, 1]. Zero means 1.0 (i.e. "unset"), so a
     * caller that wants an invisible draw must skip the call instead. */
    float opacity;
    /* Non-zero replaces every stroke width with this value in destination
     * units — the theme-weight override that keeps an icon set visually
     * interchangeable. Zero keeps the document's own widths. */
    float stroke_width;
} optics_svg_draw_opts;

/* Paint `opts->document` into `opts->canvas` or `opts->encoder`. */
OPTICS_GFX_NODISCARD OPTICS_SVG_API optics_gfx_result
optics_svg_draw(const optics_svg_draw_opts *opts);

/* ================================================================== */
/*  Headless rasterization                                            */
/* ================================================================== */

typedef enum optics_svg_pixel_format : uint32_t {
    /* Premultiplied RGBA8, row-major, tightly packed — gfx's native CPU canvas
     * layout. */
    OPTICS_SVG_PIXELS_RGBA8_PREMUL = 0,
    /* Premultiplied BGRA8 — the layout DRM/KMS hardware cursor planes and
     * Vulkan `B8G8R8A8_UNORM` sampling expect, produced by channel swap rather
     * than by a second rasterization. */
    OPTICS_SVG_PIXELS_BGRA8_PREMUL = 1,
    /* One 8-bit coverage byte per pixel — the mask-bake readback. */
    OPTICS_SVG_PIXELS_COVERAGE = 2,
} optics_svg_pixel_format;

typedef struct optics_svg_pixmap {
    uint32_t width;
    uint32_t height;
    uint32_t stride; /* bytes per row */
    optics_svg_pixel_format format;
    uint8_t *pixels; /* owned by the pixmap; free with optics_svg_pixmap_deinit */
} optics_svg_pixmap;

typedef struct optics_svg_rasterize_opts {
    const optics_svg_document *document;
    uint32_t width; /* output extent in physical pixels; both must be non-zero */
    uint32_t height;
    optics_svg_pixel_format format;
    optics_svg_paint_mode paint;
    optics_gfx_color color; /* theme / monochrome colour, as in optics_svg_draw */
    float stroke_width;     /* as in optics_svg_draw */
} optics_svg_rasterize_opts;

/* Rasterize a document with the headless software canvas. No device, surface or
 * GPU is involved, so this is safe on a worker thread and in CI. */
OPTICS_GFX_NODISCARD OPTICS_SVG_API optics_gfx_result
optics_svg_rasterize(const optics_svg_rasterize_opts *opts, optics_svg_pixmap *out);

/* Release `pixmap->pixels` and zero the struct. Safe on an already-empty
 * pixmap. */
OPTICS_SVG_API void optics_svg_pixmap_deinit(optics_svg_pixmap *pixmap);

#ifdef __cplusplus
}
#endif

#endif /* OPTICS_SVG_H */
