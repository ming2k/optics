/*
 * internal.h — private types and helpers for libreticle.
 *
 * Nothing here is installed. The public surface is <reticle/reticle.h> only.
 */

#ifndef RETICLE_INTERNAL_H
#define RETICLE_INTERNAL_H

#include <reticle/reticle.h>

#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/*  Allocation — device-less by design (ADR-0136)                     */
/* ------------------------------------------------------------------ */

void *reticle_priv_alloc(size_t bytes);
void *reticle_priv_realloc(void *ptr, size_t bytes);
void reticle_priv_free(void *ptr);
char *reticle_priv_strndup(const char *s, size_t n);

/* Find `needle` within `[hay, hay + hay_len)`, or null if absent. Unlike
 * strstr(), this never reads at or past `hay + hay_len`: the parser operates on
 * a caller-owned span that is not necessarily NUL-terminated (an archive slice,
 * an IPC payload), so a strstr() here runs off the end of the buffer looking for
 * a terminator that does not exist. */
const char *reticle_priv_find(const char *hay, size_t hay_len, const char *needle);

/* ------------------------------------------------------------------ */
/*  Growable buffers                                                  */
/* ------------------------------------------------------------------ */

#define RETICLE_INTERNAL_DECLARE_BUF(name, type)                                                   \
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

RETICLE_INTERNAL_DECLARE_BUF(reticle_cmd_buf, reticle_path_cmd);

/* ------------------------------------------------------------------ */
/*  2D affine transform (SVG matrix order: a b c d e f)               */
/* ------------------------------------------------------------------ */

typedef struct reticle_transform {
    float a, b, c, d, e, f;
} reticle_transform;

reticle_transform reticle_priv_xform_identity(void);
reticle_transform reticle_priv_xform_mul(reticle_transform lhs, reticle_transform rhs);
reticle_transform reticle_priv_xform_scale(float sx, float sy);
reticle_transform reticle_priv_xform_translate(float tx, float ty);
void reticle_priv_xform_point(reticle_transform t, float x, float y, float *ox, float *oy);
bool reticle_priv_xform_is_identity(reticle_transform t);

/* ------------------------------------------------------------------ */
/*  Raw (unresolved) document model — what the parser produces        */
/* ------------------------------------------------------------------ */

#define RETICLE_INTERNAL_MAX_STOPS FLUX_GRADIENT_MAX_STOPS

typedef enum reticle_gradient_kind : uint8_t {
    RETICLE_INTERNAL_GRADIENT_LINEAR = 0,
    RETICLE_INTERNAL_GRADIENT_RADIAL = 1,
} reticle_gradient_kind;

typedef enum reticle_gradient_units : uint8_t {
    RETICLE_INTERNAL_UNITS_OBJECT_BBOX = 0,
    RETICLE_INTERNAL_UNITS_USER_SPACE = 1,
} reticle_gradient_units;

typedef struct reticle_stop {
    float offset;
    uint8_t r, g, b, a; /* straight alpha */
} reticle_stop;

/* Which geometry attributes were written on a gradient. Absent ones inherit
 * from the href chain, so presence is tracked, not defaulted early. */
typedef enum reticle_gradient_field : uint32_t {
    RETICLE_INTERNAL_FIELD_X1 = 1u << 0,
    RETICLE_INTERNAL_FIELD_Y1 = 1u << 1,
    RETICLE_INTERNAL_FIELD_X2 = 1u << 2,
    RETICLE_INTERNAL_FIELD_Y2 = 1u << 3,
    RETICLE_INTERNAL_FIELD_CX = 1u << 4,
    RETICLE_INTERNAL_FIELD_CY = 1u << 5,
    RETICLE_INTERNAL_FIELD_R = 1u << 6,
    RETICLE_INTERNAL_FIELD_UNITS = 1u << 7,
    RETICLE_INTERNAL_FIELD_TRANSFORM = 1u << 8,
} reticle_gradient_field;

typedef struct reticle_raw_gradient {
    char *id;
    char *href; /* inherited stops/geometry source, resolved in a later pass */
    reticle_gradient_kind kind;
    reticle_gradient_units units;
    reticle_transform transform;
    uint32_t present; /* reticle_gradient_field bits */
    float x1, y1, x2, y2;
    float cx, cy, r;
    uint32_t stop_count;
    reticle_stop stops[RETICLE_INTERNAL_MAX_STOPS];
    /* Spread is parsed only to be able to say so: flux gradients pad. */
    bool spread_reflect_or_repeat;
} reticle_raw_gradient;

typedef struct reticle_raw_paint {
    reticle_paint_kind kind;
    uint8_t r, g, b, a; /* straight, for RETICLE_PAINT_SOLID */
    float opacity;      /* fill-opacity / stroke-opacity at this element */
    char *gradient_id;  /* owned; only for the gradient kinds */
} reticle_raw_paint;

typedef struct reticle_raw_shape {
    uint32_t cmd_first;
    uint32_t cmd_count;
    reticle_raw_paint fill;
    reticle_raw_paint stroke;
    float stroke_width;
    float miter_limit;
    flux_line_cap cap;
    flux_line_join join;
    flux_fill_rule fill_rule;
    float opacity; /* cascaded element opacity */
    flux_rect bounds;
} reticle_raw_shape;

typedef struct reticle_raw_attr {
    char *name;
    char *value;
} reticle_raw_attr;

/* One inheritance frame. `alpha` is the product of ancestors' opacity, which is
 * what turns per-element `opacity` into the cascaded value on the shape. */
typedef struct reticle_style {
    reticle_raw_paint fill;
    reticle_raw_paint stroke;
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
    reticle_transform ctm;
} reticle_style;

/* ------------------------------------------------------------------ */
/*  Document                                                          */
/* ------------------------------------------------------------------ */

typedef struct reticle_gradient {
    uint32_t stop_count;
    flux_gradient_stop stops[RETICLE_INTERNAL_MAX_STOPS];
    flux_point start;
    flux_point end;
    float radius;
} reticle_gradient;

struct reticle_document {
    uint32_t refs; /* atomic */
    flux_rect viewport;
    reticle_path_cmd *cmds;
    uint32_t cmd_count;
    reticle_raw_shape *raw_shapes;
    uint32_t shape_count;
    reticle_shape_view *shapes; /* resolved paint, points into `cmds`/`gradients` */
    reticle_gradient *gradients;
    uint32_t gradient_count;
    reticle_raw_attr *attrs;
    uint32_t attr_count;
    /* Unresolved gradient table, freed once resolution completes. */
    reticle_raw_gradient *raw_gradients;
    uint32_t raw_gradient_count;
};

/* ------------------------------------------------------------------ */
/*  Parser seams (parse.c / paint.c / path.c)                         */
/* ------------------------------------------------------------------ */

typedef struct reticle_parser reticle_parser;

/* Element dispatch, defined in parse.c and called by path.c's geometry emitters
 * through the parser's output buffers. */
bool reticle_priv_emit_shape(reticle_parser *p, const reticle_raw_shape *shape);

/* path.c: parse a `d` attribute and append its commands to `out`. */
bool reticle_priv_parse_path_data(const char *d, size_t len, reticle_transform ctm,
                                  reticle_cmd_buf *out, flux_rect *bounds);

/* path.c: decompose a basic shape into `out` commands. Each returns false on a
 * malformed attribute value. `override_width`/`override_height` are the
 * viewport-relative lengths the caller already resolved (0 = unused). */
bool reticle_priv_emit_rect(reticle_cmd_buf *out, reticle_transform ctm, float x, float y, float w,
                            float h, float rx, float ry, flux_rect *bounds);
bool reticle_priv_emit_ellipse(reticle_cmd_buf *out, reticle_transform ctm, float cx, float cy,
                               float rx, float ry, flux_rect *bounds);
bool reticle_priv_emit_line(reticle_cmd_buf *out, reticle_transform ctm, float x1, float y1,
                            float x2, float y2, flux_rect *bounds);
bool reticle_priv_emit_poly(reticle_cmd_buf *out, reticle_transform ctm, const float *xy,
                            uint32_t points, bool closed, flux_rect *bounds);

/* paint.c */
bool reticle_priv_parse_color(const char *value, size_t len, uint8_t *r, uint8_t *g, uint8_t *b,
                              uint8_t *a);
bool reticle_priv_parse_transform(const char *value, size_t len, reticle_transform *out);
bool reticle_priv_parse_gradient_ref(const char *value, size_t len, char **id_out);
float reticle_priv_parse_number(const char *value, size_t len, bool *ok);
/* Length with unit handling. `percent_ref` is the px length a percentage
 * resolves against (0 => percentages are unsupported here and fail). */
bool reticle_priv_parse_length(const char *value, size_t len, float percent_ref, float *out);

/* Resolve every shape's paint against the raw gradient table. Called once at the
 * end of parsing. */
bool reticle_priv_resolve_paints(reticle_document *doc);

/* Intern a resolved gradient, returning its index. Deduplicates exact matches so
 * one shared gradient used by N identically-bounded shapes costs one entry. */
bool reticle_priv_intern_gradient(reticle_document *doc, const reticle_gradient *gradient,
                                  uint32_t *index_out);

#endif /* RETICLE_INTERNAL_H */
