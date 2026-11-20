/*
 * internal.h — private types and helpers for libreticle.
 *
 * Nothing here is installed. The public surface is <reticle/reticle.h> only.
 */

#ifndef OPTICS_SVG_INTERNAL_H
#define OPTICS_SVG_INTERNAL_H

#include <optics/svg.h>

#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/*  Allocation — device-less by design (ADR-0136)                     */
/* ------------------------------------------------------------------ */

void *optics_svg_priv_alloc(size_t bytes);
void *optics_svg_priv_realloc(void *ptr, size_t bytes);
void optics_svg_priv_free(void *ptr);
char *optics_svg_priv_strndup(const char *s, size_t n);

/* Find `needle` within `[hay, hay + hay_len)`, or null if absent. Unlike
 * strstr(), this never reads at or past `hay + hay_len`: the parser operates on
 * a caller-owned span that is not necessarily NUL-terminated (an archive slice,
 * an IPC payload), so a strstr() here runs off the end of the buffer looking for
 * a terminator that does not exist. */
const char *optics_svg_priv_find(const char *hay, size_t hay_len, const char *needle);

/* ------------------------------------------------------------------ */
/*  Growable buffers                                                  */
/* ------------------------------------------------------------------ */

#define OPTICS_SVG_INTERNAL_DECLARE_BUF(name, type)                                                \
    typedef struct name {                                                                          \
        type *data;                                                                                \
        uint32_t count;                                                                            \
        uint32_t cap;                                                                              \
        /* Ceiling on `count`, 0 = unbounded. Set from `reticle_parse_limits` so the               \
         * limit is enforced as commands are produced, not after the element that                  \
         * produced them has already allocated. */                                                 \
        uint32_t limit;                                                                            \
    } name;                                                                                        \
    bool name##_push(name *b, type value);                                                         \
    void name##_deinit(name *b)

OPTICS_SVG_INTERNAL_DECLARE_BUF(optics_svg_cmd_buf, optics_svg_path_cmd);

/* ------------------------------------------------------------------ */
/*  2D affine transform (SVG matrix order: a b c d e f)               */
/* ------------------------------------------------------------------ */

typedef struct optics_svg_transform {
    float a, b, c, d, e, f;
} optics_svg_transform;

optics_svg_transform optics_svg_priv_xform_identity(void);
optics_svg_transform optics_svg_priv_xform_mul(optics_svg_transform lhs, optics_svg_transform rhs);
optics_svg_transform optics_svg_priv_xform_scale(float sx, float sy);
optics_svg_transform optics_svg_priv_xform_translate(float tx, float ty);
void optics_svg_priv_xform_point(optics_svg_transform t, float x, float y, float *ox, float *oy);
bool optics_svg_priv_xform_is_identity(optics_svg_transform t);

/* ------------------------------------------------------------------ */
/*  Raw (unresolved) document model — what the parser produces        */
/* ------------------------------------------------------------------ */

#define OPTICS_SVG_INTERNAL_MAX_STOPS OPTICS_GFX_GRADIENT_MAX_STOPS

typedef enum optics_svg_gradient_kind : uint8_t {
    OPTICS_SVG_INTERNAL_GRADIENT_LINEAR = 0,
    OPTICS_SVG_INTERNAL_GRADIENT_RADIAL = 1,
} optics_svg_gradient_kind;

typedef enum optics_svg_gradient_units : uint8_t {
    OPTICS_SVG_INTERNAL_UNITS_OBJECT_BBOX = 0,
    OPTICS_SVG_INTERNAL_UNITS_USER_SPACE = 1,
} optics_svg_gradient_units;

typedef struct optics_svg_stop {
    float offset;
    uint8_t r, g, b, a; /* straight alpha */
} optics_svg_stop;

/* Which geometry attributes were written on a gradient. Absent ones inherit
 * from the href chain, so presence is tracked, not defaulted early. */
typedef enum optics_svg_gradient_field : uint32_t {
    OPTICS_SVG_INTERNAL_FIELD_X1 = 1u << 0,
    OPTICS_SVG_INTERNAL_FIELD_Y1 = 1u << 1,
    OPTICS_SVG_INTERNAL_FIELD_X2 = 1u << 2,
    OPTICS_SVG_INTERNAL_FIELD_Y2 = 1u << 3,
    OPTICS_SVG_INTERNAL_FIELD_CX = 1u << 4,
    OPTICS_SVG_INTERNAL_FIELD_CY = 1u << 5,
    OPTICS_SVG_INTERNAL_FIELD_R = 1u << 6,
    OPTICS_SVG_INTERNAL_FIELD_UNITS = 1u << 7,
    OPTICS_SVG_INTERNAL_FIELD_TRANSFORM = 1u << 8,
} optics_svg_gradient_field;

typedef struct optics_svg_raw_gradient {
    char *id;
    char *href; /* inherited stops/geometry source, resolved in a later pass */
    optics_svg_gradient_kind kind;
    optics_svg_gradient_units units;
    optics_svg_transform transform;
    uint32_t present; /* reticle_gradient_field bits */
    float x1, y1, x2, y2;
    float cx, cy, r;
    uint32_t stop_count;
    optics_svg_stop stops[OPTICS_SVG_INTERNAL_MAX_STOPS];
    /* Spread is parsed only to be able to say so: flux gradients pad. */
    bool spread_reflect_or_repeat;
} optics_svg_raw_gradient;

typedef struct optics_svg_raw_paint {
    optics_svg_paint_kind kind;
    uint8_t r, g, b, a; /* straight, for RETICLE_PAINT_SOLID */
    float opacity;      /* fill-opacity / stroke-opacity at this element */
    char *gradient_id;  /* owned; only for the gradient kinds */
} optics_svg_raw_paint;

typedef struct optics_svg_raw_shape {
    uint32_t cmd_first;
    uint32_t cmd_count;
    optics_svg_raw_paint fill;
    optics_svg_raw_paint stroke;
    float stroke_width;
    float miter_limit;
    optics_gfx_line_cap cap;
    optics_gfx_line_join join;
    optics_gfx_fill_rule fill_rule;
    float opacity; /* cascaded element opacity */
    optics_gfx_rect bounds;
} optics_svg_raw_shape;

typedef struct optics_svg_raw_attr {
    char *name;
    char *value;
} optics_svg_raw_attr;

/* One inheritance frame. `alpha` is the product of ancestors' opacity, which is
 * what turns per-element `opacity` into the cascaded value on the shape. */
typedef struct optics_svg_style {
    optics_svg_raw_paint fill;
    optics_svg_raw_paint stroke;
    float stroke_width;
    float miter_limit;
    optics_gfx_line_cap cap;
    optics_gfx_line_join join;
    optics_gfx_fill_rule fill_rule;
    float fill_opacity;
    float stroke_opacity;
    float alpha;
    bool has_color;
    uint8_t color_r, color_g, color_b, color_a;
    optics_svg_transform ctm;
} optics_svg_style;

/* ------------------------------------------------------------------ */
/*  Document                                                          */
/* ------------------------------------------------------------------ */

typedef struct optics_svg_gradient {
    uint32_t stop_count;
    optics_gfx_gradient_stop stops[OPTICS_SVG_INTERNAL_MAX_STOPS];
    optics_gfx_point start;
    optics_gfx_point end;
    float radius;
} optics_svg_gradient;

struct optics_svg_document {
    uint32_t refs; /* atomic */
    optics_gfx_rect viewport;
    optics_svg_path_cmd *cmds;
    uint32_t cmd_count;
    optics_svg_raw_shape *raw_shapes;
    uint32_t shape_count;
    optics_svg_shape_view *shapes; /* resolved paint, points into `cmds`/`gradients` */
    optics_svg_gradient *gradients;
    uint32_t gradient_count;
    optics_svg_raw_attr *attrs;
    uint32_t attr_count;
    /* Unresolved gradient table, freed once resolution completes. */
    optics_svg_raw_gradient *raw_gradients;
    uint32_t raw_gradient_count;
};

/* ------------------------------------------------------------------ */
/*  Parser seams (parse.c / paint.c / path.c)                         */
/* ------------------------------------------------------------------ */

typedef struct optics_svg_parser optics_svg_parser;

/* Element dispatch, defined in parse.c and called by path.c's geometry emitters
 * through the parser's output buffers. */
bool optics_svg_priv_emit_shape(optics_svg_parser *p, const optics_svg_raw_shape *shape);

/* path.c: parse a `d` attribute and append its commands to `out`. */
bool optics_svg_priv_parse_path_data(const char *d, size_t len, optics_svg_transform ctm,
                                     optics_svg_cmd_buf *out, optics_gfx_rect *bounds);

/* path.c: decompose a basic shape into `out` commands. Each returns false on a
 * malformed attribute value. `override_width`/`override_height` are the
 * viewport-relative lengths the caller already resolved (0 = unused). */
bool optics_svg_priv_emit_rect(optics_svg_cmd_buf *out, optics_svg_transform ctm, float x, float y,
                               float w, float h, float rx, float ry, optics_gfx_rect *bounds);
bool optics_svg_priv_emit_ellipse(optics_svg_cmd_buf *out, optics_svg_transform ctm, float cx,
                                  float cy, float rx, float ry, optics_gfx_rect *bounds);
bool optics_svg_priv_emit_line(optics_svg_cmd_buf *out, optics_svg_transform ctm, float x1,
                               float y1, float x2, float y2, optics_gfx_rect *bounds);
bool optics_svg_priv_emit_poly(optics_svg_cmd_buf *out, optics_svg_transform ctm, const float *xy,
                               uint32_t points, bool closed, optics_gfx_rect *bounds);

/* paint.c */
bool optics_svg_priv_parse_color(const char *value, size_t len, uint8_t *r, uint8_t *g, uint8_t *b,
                                 uint8_t *a);
bool optics_svg_priv_parse_transform(const char *value, size_t len, optics_svg_transform *out);
bool optics_svg_priv_parse_gradient_ref(const char *value, size_t len, char **id_out);
float optics_svg_priv_parse_number(const char *value, size_t len, bool *ok);
/* Length with unit handling. `percent_ref` is the px length a percentage
 * resolves against (0 => percentages are unsupported here and fail). */
bool optics_svg_priv_parse_length(const char *value, size_t len, float percent_ref, float *out);

/* Resolve every shape's paint against the raw gradient table. Called once at the
 * end of parsing. */
bool optics_svg_priv_resolve_paints(optics_svg_document *doc);

/* Intern a resolved gradient, returning its index. Deduplicates exact matches so
 * one shared gradient used by N identically-bounded shapes costs one entry. */
bool optics_svg_priv_intern_gradient(optics_svg_document *doc, const optics_svg_gradient *gradient,
                                     uint32_t *index_out);

#endif /* RETICLE_INTERNAL_H */
