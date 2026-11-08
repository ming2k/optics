/*
 * internal.h — private types and helpers for libsvg.
 *
 * Nothing here is installed. The public surface is <svg/svg.h> only.
 */

#ifndef SVG_INTERNAL_H
#define SVG_INTERNAL_H

#include <svg/svg.h>

#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/*  Allocation — device-less by design (ADR-0136)                     */
/* ------------------------------------------------------------------ */

void *svg__alloc(size_t bytes);
void *svg__realloc(void *ptr, size_t bytes);
void svg__free(void *ptr);
char *svg__strndup(const char *s, size_t n);

/* Find `needle` within `[hay, hay + hay_len)`, or null if absent. Unlike
 * strstr(), this never reads at or past `hay + hay_len`: the parser operates on
 * a caller-owned span that is not necessarily NUL-terminated (an archive slice,
 * an IPC payload), so a strstr() here runs off the end of the buffer looking for
 * a terminator that does not exist. */
const char *svg__find(const char *hay, size_t hay_len, const char *needle);

/* ------------------------------------------------------------------ */
/*  Growable buffers                                                  */
/* ------------------------------------------------------------------ */

#define SVG__DECLARE_BUF(name, type)                                                               \
    typedef struct name {                                                                          \
        type *data;                                                                                \
        uint32_t count;                                                                            \
        uint32_t cap;                                                                              \
        /* Ceiling on `count`, 0 = unbounded. Set from `svg_parse_limits` so the                   \
         * limit is enforced as commands are produced, not after the element that                  \
         * produced them has already allocated. */                                                 \
        uint32_t limit;                                                                            \
    } name;                                                                                        \
    bool name##_push(name *b, type value);                                                         \
    void name##_deinit(name *b)

SVG__DECLARE_BUF(svg_cmd_buf, svg_path_cmd);

/* ------------------------------------------------------------------ */
/*  2D affine transform (SVG matrix order: a b c d e f)               */
/* ------------------------------------------------------------------ */

typedef struct svg_transform {
    float a, b, c, d, e, f;
} svg_transform;

svg_transform svg__xform_identity(void);
svg_transform svg__xform_mul(svg_transform lhs, svg_transform rhs);
svg_transform svg__xform_scale(float sx, float sy);
svg_transform svg__xform_translate(float tx, float ty);
void svg__xform_point(svg_transform t, float x, float y, float *ox, float *oy);
bool svg__xform_is_identity(svg_transform t);

/* ------------------------------------------------------------------ */
/*  Raw (unresolved) document model — what the parser produces        */
/* ------------------------------------------------------------------ */

#define SVG__MAX_STOPS FLUX_GRADIENT_MAX_STOPS

typedef enum svg_gradient_kind : uint8_t {
    SVG__GRADIENT_LINEAR = 0,
    SVG__GRADIENT_RADIAL = 1,
} svg_gradient_kind;

typedef enum svg_gradient_units : uint8_t {
    SVG__UNITS_OBJECT_BBOX = 0,
    SVG__UNITS_USER_SPACE = 1,
} svg_gradient_units;

typedef struct svg_stop {
    float offset;
    uint8_t r, g, b, a; /* straight alpha */
} svg_stop;

/* Which geometry attributes were written on a gradient. Absent ones inherit
 * from the href chain, so presence is tracked, not defaulted early. */
typedef enum svg_gradient_field : uint32_t {
    SVG__FIELD_X1 = 1u << 0,
    SVG__FIELD_Y1 = 1u << 1,
    SVG__FIELD_X2 = 1u << 2,
    SVG__FIELD_Y2 = 1u << 3,
    SVG__FIELD_CX = 1u << 4,
    SVG__FIELD_CY = 1u << 5,
    SVG__FIELD_R = 1u << 6,
    SVG__FIELD_UNITS = 1u << 7,
    SVG__FIELD_TRANSFORM = 1u << 8,
} svg_gradient_field;

typedef struct svg_raw_gradient {
    char *id;
    char *href; /* inherited stops/geometry source, resolved in a later pass */
    svg_gradient_kind kind;
    svg_gradient_units units;
    svg_transform transform;
    uint32_t present; /* svg_gradient_field bits */
    float x1, y1, x2, y2;
    float cx, cy, r;
    uint32_t stop_count;
    svg_stop stops[SVG__MAX_STOPS];
    /* Spread is parsed only to be able to say so: flux gradients pad. */
    bool spread_reflect_or_repeat;
} svg_raw_gradient;

typedef struct svg_raw_paint {
    svg_paint_kind kind;
    uint8_t r, g, b, a; /* straight, for SVG_PAINT_SOLID */
    float opacity;      /* fill-opacity / stroke-opacity at this element */
    char *gradient_id;  /* owned; only for the gradient kinds */
} svg_raw_paint;

typedef struct svg_raw_shape {
    uint32_t cmd_first;
    uint32_t cmd_count;
    svg_raw_paint fill;
    svg_raw_paint stroke;
    float stroke_width;
    float miter_limit;
    flux_line_cap cap;
    flux_line_join join;
    flux_fill_rule fill_rule;
    float opacity; /* cascaded element opacity */
    flux_rect bounds;
} svg_raw_shape;

typedef struct svg_raw_attr {
    char *name;
    char *value;
} svg_raw_attr;

/* One inheritance frame. `alpha` is the product of ancestors' opacity, which is
 * what turns per-element `opacity` into the cascaded value on the shape. */
typedef struct svg_style {
    svg_raw_paint fill;
    svg_raw_paint stroke;
    float stroke_width;
    float miter_limit;
    flux_line_cap cap;
    flux_line_join join;
    flux_fill_rule fill_rule;
    float fill_opacity;
    float stroke_opacity;
    float alpha;
    bool has_color;
    uint8_t color_r, color_g, color_b, color_a;
    svg_transform ctm;
} svg_style;

/* ------------------------------------------------------------------ */
/*  Document                                                          */
/* ------------------------------------------------------------------ */

typedef struct svg_gradient {
    uint32_t stop_count;
    flux_gradient_stop stops[SVG__MAX_STOPS];
    flux_point start;
    flux_point end;
    float radius;
} svg_gradient;

struct svg_document {
    uint32_t refs; /* atomic */
    flux_rect viewport;
    svg_path_cmd *cmds;
    uint32_t cmd_count;
    svg_raw_shape *raw_shapes;
    uint32_t shape_count;
    svg_shape_view *shapes; /* resolved paint, points into `cmds`/`gradients` */
    svg_gradient *gradients;
    uint32_t gradient_count;
    svg_raw_attr *attrs;
    uint32_t attr_count;
    /* Unresolved gradient table, freed once resolution completes. */
    svg_raw_gradient *raw_gradients;
    uint32_t raw_gradient_count;
};

/* ------------------------------------------------------------------ */
/*  Parser seams (parse.c / paint.c / path.c)                         */
/* ------------------------------------------------------------------ */

typedef struct svg_parser svg_parser;

/* Element dispatch, defined in parse.c and called by path.c's geometry emitters
 * through the parser's output buffers. */
bool svg__emit_shape(svg_parser *p, const svg_raw_shape *shape);

/* path.c: parse a `d` attribute and append its commands to `out`. */
bool svg__parse_path_data(const char *d, size_t len, svg_transform ctm, svg_cmd_buf *out,
                          flux_rect *bounds);

/* path.c: decompose a basic shape into `out` commands. Each returns false on a
 * malformed attribute value. `override_width`/`override_height` are the
 * viewport-relative lengths the caller already resolved (0 = unused). */
bool svg__emit_rect(svg_cmd_buf *out, svg_transform ctm, float x, float y, float w, float h,
                    float rx, float ry, flux_rect *bounds);
bool svg__emit_ellipse(svg_cmd_buf *out, svg_transform ctm, float cx, float cy, float rx, float ry,
                       flux_rect *bounds);
bool svg__emit_line(svg_cmd_buf *out, svg_transform ctm, float x1, float y1, float x2, float y2,
                    flux_rect *bounds);
bool svg__emit_poly(svg_cmd_buf *out, svg_transform ctm, const float *xy, uint32_t points,
                    bool closed, flux_rect *bounds);

/* paint.c */
bool svg__parse_color(const char *value, size_t len, uint8_t *r, uint8_t *g, uint8_t *b,
                      uint8_t *a);
bool svg__parse_transform(const char *value, size_t len, svg_transform *out);
bool svg__parse_gradient_ref(const char *value, size_t len, char **id_out);
float svg__parse_number(const char *value, size_t len, bool *ok);
/* Length with unit handling. `percent_ref` is the px length a percentage
 * resolves against (0 => percentages are unsupported here and fail). */
bool svg__parse_length(const char *value, size_t len, float percent_ref, float *out);

/* Resolve every shape's paint against the raw gradient table. Called once at the
 * end of parsing. */
bool svg__resolve_paints(svg_document *doc);

/* Intern a resolved gradient, returning its index. Deduplicates exact matches so
 * one shared gradient used by N identically-bounded shapes costs one entry. */
bool svg__intern_gradient(svg_document *doc, const svg_gradient *gradient, uint32_t *index_out);

#endif /* SVG_INTERNAL_H */
