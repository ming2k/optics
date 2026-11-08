/* parse.c — the document parser: markup scanning, style cascade, element
 * dispatch, and gradient collection.
 *
 * The scanner is deliberately a *tag* scanner rather than an XML parser. SVG
 * consumed by this library is static vector art: it has no DTD, no entity
 * expansion, no namespaces it needs to resolve, and no CDATA-bearing content
 * model. A tag scanner therefore cannot be attacked through the classes an XML
 * processor can (billion laughs, external entities, quadratic namespace
 * resolution), and it is what keeps this library dependency-free.
 *
 * Every ceiling is explicit (svg_parse_limits): element count, nesting depth,
 * shape count, command count, gradient count, and total input bytes. A document
 * that exceeds one fails to parse; it never yields a truncated document. */

#include "internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum {
    SVG__DEFAULT_MAX_SHAPES = 4096,
    SVG__DEFAULT_MAX_COMMANDS = 65536,
    SVG__DEFAULT_MAX_GRADIENTS = 256,
    SVG__DEFAULT_MAX_ELEMENTS = 16384,
    SVG__DEFAULT_MAX_DEPTH = 32,
    SVG__MAX_INPUT_BYTES = 16u << 20,
    SVG__MAX_STYLE_DEPTH = 64,
};

/* Live documents, for the leak lane (INV-ARCH-LEDGER). Atomic because
 * svg_retain/svg_release are documented as callable from any thread. */
static uint32_t g_live_documents;

uint32_t svg_live_documents(void) {
    return __atomic_load_n(&g_live_documents, __ATOMIC_RELAXED);
}

/* ------------------------------------------------------------------ */
/*  Tokeniser                                                         */
/* ------------------------------------------------------------------ */

typedef struct svg_attr {
    const char *name;
    size_t name_len;
    const char *value;
    size_t value_len;
} svg_attr;

typedef struct svg_tag {
    const char *name;
    size_t name_len;
    svg_attr attrs[64];
    uint32_t attr_count;
    bool self_closing;
    bool is_end;
} svg_tag;

static bool is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static bool name_eq(const char *name, size_t len, const char *literal) {
    size_t n = strlen(literal);
    if (n != len)
        return false;
    for (size_t i = 0; i < n; ++i) {
        char c = name[i];
        /* Element names are case-sensitive in SVG; only the ASCII namespace
         * prefix is normalised away, so an exact compare is correct. */
        if (c != literal[i])
            return false;
    }
    return true;
}

/* Read one element tag starting at `*cursor` (which points at '<'). */
static bool read_tag(const char **cursor, const char *end, svg_tag *tag) {
    const char *p = *cursor;
    if (p >= end || *p != '<')
        return false;
    p++;
    *tag = (svg_tag){};
    if (p < end && *p == '/') {
        tag->is_end = true;
        p++;
    }
    const char *name_start = p;
    while (p < end && !is_ws(*p) && *p != '>' && *p != '/')
        p++;
    tag->name = name_start;
    tag->name_len = (size_t)(p - name_start);
    if (tag->name_len == 0)
        return false;

    while (p < end) {
        while (p < end && is_ws(*p))
            p++;
        if (p >= end)
            return false;
        if (*p == '/') {
            p++;
            if (p < end && *p == '>') {
                tag->self_closing = true;
                p++;
                *cursor = p;
                return true;
            }
            continue;
        }
        if (*p == '>') {
            p++;
            *cursor = p;
            return true;
        }
        const char *attr_start = p;
        while (p < end && !is_ws(*p) && *p != '=' && *p != '>' && *p != '/')
            p++;
        size_t attr_len = (size_t)(p - attr_start);
        if (attr_len == 0) {
            p++;
            continue;
        }
        while (p < end && is_ws(*p))
            p++;
        if (p >= end || *p != '=') {
            /* A valueless attribute is legal in HTML, not in SVG. Skip it. */
            continue;
        }
        p++;
        while (p < end && is_ws(*p))
            p++;
        if (p >= end)
            return false;
        const char *value_start = nullptr;
        size_t value_len = 0;
        if (*p == '"' || *p == '\'') {
            char quote = *p++;
            value_start = p;
            while (p < end && *p != quote)
                p++;
            if (p >= end)
                return false;
            value_len = (size_t)(p - value_start);
            p++;
        } else {
            value_start = p;
            while (p < end && !is_ws(*p) && *p != '>')
                p++;
            value_len = (size_t)(p - value_start);
        }
        if (tag->attr_count < sizeof tag->attrs / sizeof tag->attrs[0]) {
            tag->attrs[tag->attr_count++] = (svg_attr){
                .name = attr_start,
                .name_len = attr_len,
                .value = value_start,
                .value_len = value_len,
            };
        }
    }
    return false;
}

static const svg_attr *find_attr(const svg_tag *tag, const char *name) {
    for (uint32_t i = 0; i < tag->attr_count; ++i) {
        if (name_eq(tag->attrs[i].name, tag->attrs[i].name_len, name))
            return &tag->attrs[i];
    }
    return nullptr;
}

/* A value span, trimmed. */
static bool attr_span(const svg_tag *tag, const char *name, const char **value, size_t *len) {
    const svg_attr *attr = find_attr(tag, name);
    if (!attr)
        return false;
    const char *p = attr->value;
    const char *e = p + attr->value_len;
    while (p < e && is_ws(*p))
        p++;
    while (e > p && is_ws(e[-1]))
        e--;
    *value = p;
    *len = (size_t)(e - p);
    return true;
}

static bool span_eq(const char *value, size_t len, const char *literal) {
    return name_eq(value, len, literal);
}

/* ------------------------------------------------------------------ */
/*  Parser state                                                      */
/* ------------------------------------------------------------------ */

typedef struct svg_style_stack {
    svg_style frames[SVG__MAX_STYLE_DEPTH];
    uint32_t depth;
} svg_style_stack;

struct svg_parser {
    const char *p;
    const char *end;
    svg_parse_limits limits;

    svg_cmd_buf cmds;
    svg_raw_shape *shapes;
    uint32_t shape_count;
    uint32_t shape_cap;

    svg_raw_gradient *gradients;
    uint32_t gradient_count;
    uint32_t gradient_cap;

    svg_raw_attr *attrs;
    uint32_t attr_count;
    uint32_t attr_cap;

    svg_style_stack stack;
    uint32_t element_count;
    bool saw_markup;     /* at least one tag was read */
    uint32_t style_peak; /* the deepest style frame ever pushed */
    /* Opaque-container nesting depth: >0 means geometry contributes nothing. */
    uint32_t skip_depth;
    /* A `</g>`/`</svg>` arrived with no matching open frame. Without this the
     * pop would silently consume an *ancestor* frame (the root seed at worst),
     * corrupting paint inheritance for the rest of the document while still
     * reporting success. Recorded instead, and turned into a parse failure. */
    bool style_underflow;

    /* Pending gradient: `<linearGradient>`/`<radialGradient>` open, its stops
     * being collected from `<stop>` children. */
    bool in_gradient;
    svg_raw_gradient gradient;
    bool gradient_is_pending;

    /* The root viewport, resolved once the `<svg>` tag is seen. */
    bool saw_root;
    flux_rect viewport;
    bool viewport_from_viewbox;
};

static void free_paint(svg_raw_paint *paint) {
    svg__free(paint->gradient_id);
    paint->gradient_id = nullptr;
}

static void free_gradient(svg_raw_gradient *gradient) {
    svg__free(gradient->id);
    svg__free(gradient->href);
    *gradient = (svg_raw_gradient){};
}

static void free_tag_attrs(svg_parser *p) {
    for (uint32_t i = 0; i < p->attr_count; ++i) {
        svg__free(p->attrs[i].name);
        svg__free(p->attrs[i].value);
    }
    p->attr_count = 0;
}

/* ---- Style -------------------------------------------------------- */

static svg_raw_paint paint_from_style_value(svg_parser *p, const char *value, size_t len,
                                            float opacity) {
    svg_raw_paint paint = {.kind = SVG_PAINT_NONE, .opacity = 1.0f};
    if (len == 4 && span_eq(value, len, "none")) {
        paint.kind = SVG_PAINT_NONE;
        return paint;
    }
    if (len == 12 && span_eq(value, len, "currentColor")) {
        paint.kind = SVG_PAINT_CURRENT_COLOR;
        paint.opacity = opacity;
        return paint;
    }
    if (len == 11 && span_eq(value, len, "transparent")) {
        paint.kind = SVG_PAINT_SOLID;
        paint.opacity = 0.0f;
        return paint;
    }
    char *id = nullptr;
    if (svg__parse_gradient_ref(value, len, &id)) {
        paint.kind = SVG_PAINT_LINEAR_GRADIENT; /* corrected at resolution */
        paint.gradient_id = id;
        paint.opacity = opacity;
        return paint;
    }
    uint8_t r = 0, g = 0, b = 0, a = 255;
    if (svg__parse_color(value, len, &r, &g, &b, &a)) {
        paint.kind = SVG_PAINT_SOLID;
        paint.r = r;
        paint.g = g;
        paint.b = b;
        paint.a = a;
        paint.opacity = opacity;
        return paint;
    }
    (void)p;
    return paint;
}

/* `f = 0.5` or `f = 50%`. */
static bool parse_opacity(const char *value, size_t len, float *out) {
    bool percent = len > 0 && value[len - 1] == '%';
    bool ok = false;
    float v = svg__parse_number(value, percent ? len - 1 : len, &ok);
    if (!ok)
        return false;
    if (percent)
        v /= 100.0f;
    if (v < 0.0f)
        v = 0.0f;
    if (v > 1.0f)
        v = 1.0f;
    *out = v;
    return true;
}

static flux_line_cap parse_cap(const char *value, size_t len, flux_line_cap fallback) {
    if (span_eq(value, len, "round"))
        return FLUX_CAP_ROUND;
    if (span_eq(value, len, "square"))
        return FLUX_CAP_SQUARE;
    if (span_eq(value, len, "butt"))
        return FLUX_CAP_BUTT;
    return fallback;
}

static flux_line_join parse_join(const char *value, size_t len, flux_line_join fallback) {
    if (span_eq(value, len, "round"))
        return FLUX_JOIN_ROUND;
    if (span_eq(value, len, "bevel"))
        return FLUX_JOIN_BEVEL;
    if (span_eq(value, len, "miter") || span_eq(value, len, "arcs") ||
        span_eq(value, len, "miter-clip"))
        return FLUX_JOIN_MITER;
    return fallback;
}

static flux_fill_rule parse_fill_rule(const char *value, size_t len, flux_fill_rule fallback) {
    if (span_eq(value, len, "evenodd"))
        return FLUX_FILL_EVEN_ODD;
    if (span_eq(value, len, "nonzero"))
        return FLUX_FILL_NON_ZERO;
    return fallback;
}

/* Split a `style="..."` declaration list and apply each `name:value`. */
static void apply_style_attr(svg_parser *p, svg_style *style, const char *value, size_t len) {
    (void)p;
    const char *cursor = value;
    const char *end = value + len;
    while (cursor < end) {
        const char *semi = cursor;
        while (semi < end && *semi != ';')
            semi++;
        const char *colon = cursor;
        while (colon < semi && *colon != ':')
            colon++;
        if (colon < semi) {
            const char *name = cursor;
            const char *name_end = colon;
            while (name < name_end && is_ws(*name))
                name++;
            while (name_end > name && is_ws(name_end[-1]))
                name_end--;
            const char *val = colon + 1;
            while (val < semi && is_ws(*val))
                val++;
            const char *val_end = semi;
            while (val_end > val && is_ws(val_end[-1]))
                val_end--;
            size_t name_len = (size_t)(name_end - name);
            size_t val_len = (size_t)(val_end - val);
            if (name_eq(name, name_len, "fill")) {
                free_paint(&style->fill);
                style->fill = paint_from_style_value(nullptr, val, val_len, 1.0f);
            } else if (name_eq(name, name_len, "stroke")) {
                free_paint(&style->stroke);
                style->stroke = paint_from_style_value(nullptr, val, val_len, 1.0f);
            } else if (name_eq(name, name_len, "stroke-width")) {
                float width = 0.0f;
                if (svg__parse_length(val, val_len, 0.0f, &width) && width >= 0.0f)
                    style->stroke_width = width;
            } else if (name_eq(name, name_len, "fill-opacity")) {
                float opacity = 1.0f;
                if (parse_opacity(val, val_len, &opacity))
                    style->fill_opacity = opacity;
            } else if (name_eq(name, name_len, "stroke-opacity")) {
                float opacity = 1.0f;
                if (parse_opacity(val, val_len, &opacity))
                    style->stroke_opacity = opacity;
            } else if (name_eq(name, name_len, "opacity")) {
                float opacity = 1.0f;
                if (parse_opacity(val, val_len, &opacity))
                    style->alpha *= opacity;
            } else if (name_eq(name, name_len, "fill-rule")) {
                style->fill_rule = parse_fill_rule(val, val_len, style->fill_rule);
            } else if (name_eq(name, name_len, "stroke-linecap")) {
                style->cap = parse_cap(val, val_len, style->cap);
            } else if (name_eq(name, name_len, "stroke-linejoin")) {
                style->join = parse_join(val, val_len, style->join);
            } else if (name_eq(name, name_len, "stroke-miterlimit")) {
                bool ok = false;
                float limit = svg__parse_number(val, val_len, &ok);
                if (ok && limit > 0.0f)
                    style->miter_limit = limit;
            }
        }
        cursor = semi < end ? semi + 1 : end;
    }
}

/* Apply presentation attributes then `style=""` (the cascade order CSS
 * mandates: presentation attributes are author-level, style is stronger). */
static void apply_tag_style(svg_parser *p, svg_style *style, const svg_tag *tag) {
    const char *value = nullptr;
    size_t len = 0;
    if (attr_span(tag, "fill", &value, &len)) {
        free_paint(&style->fill);
        style->fill = paint_from_style_value(p, value, len, 1.0f);
    }
    if (attr_span(tag, "stroke", &value, &len)) {
        free_paint(&style->stroke);
        style->stroke = paint_from_style_value(p, value, len, 1.0f);
    }
    if (attr_span(tag, "color", &value, &len)) {
        uint8_t r = 0, g = 0, b = 0, a = 255;
        if (svg__parse_color(value, len, &r, &g, &b, &a)) {
            style->has_color = true;
            style->color_r = r;
            style->color_g = g;
            style->color_b = b;
            style->color_a = a;
        }
    }
    if (attr_span(tag, "stroke-width", &value, &len)) {
        float width = 0.0f;
        if (svg__parse_length(value, len, 0.0f, &width) && width >= 0.0f)
            style->stroke_width = width;
    }
    if (attr_span(tag, "fill-opacity", &value, &len)) {
        float opacity = 1.0f;
        if (parse_opacity(value, len, &opacity))
            style->fill_opacity = opacity;
    }
    if (attr_span(tag, "stroke-opacity", &value, &len)) {
        float opacity = 1.0f;
        if (parse_opacity(value, len, &opacity))
            style->stroke_opacity = opacity;
    }
    if (attr_span(tag, "opacity", &value, &len)) {
        float opacity = 1.0f;
        if (parse_opacity(value, len, &opacity))
            style->alpha *= opacity;
    }
    if (attr_span(tag, "fill-rule", &value, &len))
        style->fill_rule = parse_fill_rule(value, len, style->fill_rule);
    if (attr_span(tag, "stroke-linecap", &value, &len))
        style->cap = parse_cap(value, len, style->cap);
    if (attr_span(tag, "stroke-linejoin", &value, &len))
        style->join = parse_join(value, len, style->join);
    if (attr_span(tag, "stroke-miterlimit", &value, &len)) {
        bool ok = false;
        float limit = svg__parse_number(value, len, &ok);
        if (ok && limit > 0.0f)
            style->miter_limit = limit;
    }
    if (attr_span(tag, "style", &value, &len))
        apply_style_attr(p, style, value, len);
}

/* Defined below `style_push`/`style_pop`, which dispose the frames they pop. */
static void style_dispose(svg_style *style);

/* Takes ownership of `style` on success: the frame is disposed by `style_pop`
 * when it is popped, so a caller must not dispose it again. On failure the
 * caller still owns it. (A frame can own a heap `gradient_id` — `<g
 * fill="url(#g)">` clones the reference — so a pop that only decremented the
 * depth leaked it.) */
static bool style_push(svg_parser *p, svg_style *style) {
    /* Frame 0 is the root seed, so `max_depth` nested containers fit on top of
     * it. The hard array bound is the second, independent ceiling. */
    if (p->stack.depth >= p->limits.max_depth + 1u)
        return false;
    if (p->stack.depth >= SVG__MAX_STYLE_DEPTH)
        return false;
    p->stack.frames[p->stack.depth++] = *style;
    if (p->stack.depth > p->style_peak)
        p->style_peak = p->stack.depth;
    return true;
}

/* Pops the top style frame and disposes it. Returns false when the stack holds
 * only the root seed (depth <= 1) — i.e. the caller's close tag had no matching
 * open — so an underflow is reported rather than silently swallowing an
 * ancestor frame. */
static bool style_pop(svg_parser *p) {
    if (p->stack.depth <= 1)
        return false;
    p->stack.depth--;
    style_dispose(&p->stack.frames[p->stack.depth]);
    return true;
}

static const svg_style *style_current(const svg_parser *p) {
    static const svg_style root = {
        .fill = {.kind = SVG_PAINT_NONE, .opacity = 1.0f},
        .stroke = {.kind = SVG_PAINT_NONE, .opacity = 1.0f},
        .stroke_width = 1.0f,
        .miter_limit = 4.0f,
        .cap = FLUX_CAP_BUTT,
        .join = FLUX_JOIN_MITER,
        .fill_rule = FLUX_FILL_NON_ZERO,
        .fill_opacity = 1.0f,
        .stroke_opacity = 1.0f,
        .alpha = 1.0f,
        .ctm = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f},
    };
    return p->stack.depth ? &p->stack.frames[p->stack.depth - 1] : &root;
}

/* The initial SVG fill is black; a shape with neither fill nor stroke paints
 * black fill. Encode that as explicit defaults on the root svg element. */
static void seed_root_style(svg_parser *p, svg_style *style) {
    style->fill = (svg_raw_paint){.kind = SVG_PAINT_SOLID,
                                  .r = 0,
                                  .g = 0,
                                  .b = 0,
                                  .a = 255,
                                  .opacity = 1.0f,
                                  .gradient_id = nullptr};
    style->stroke = (svg_raw_paint){.kind = SVG_PAINT_NONE, .opacity = 1.0f};
    style->stroke_width = 1.0f;
    style->miter_limit = 4.0f;
    style->cap = FLUX_CAP_BUTT;
    style->join = FLUX_JOIN_MITER;
    style->fill_rule = FLUX_FILL_NON_ZERO;
    style->fill_opacity = 1.0f;
    style->stroke_opacity = 1.0f;
    style->alpha = 1.0f;
    /* No `color` was declared, so `currentColor` stays unresolved: it is the
     * caller's theme colour, and only this library's consumer knows it. */
    style->has_color = false;
    style->color_r = 0;
    style->color_g = 0;
    style->color_b = 0;
    style->color_a = 255;
    style->ctm = svg__xform_identity();
    (void)p;
}

/* Duplicate a style frame, deep-copying the two paints' owned ids. */
static bool style_clone(const svg_style *src, svg_style *dst) {
    *dst = *src;
    dst->fill.gradient_id = src->fill.gradient_id
                                ? svg__strndup(src->fill.gradient_id, strlen(src->fill.gradient_id))
                                : nullptr;
    if (src->fill.gradient_id && !dst->fill.gradient_id)
        return false;
    dst->stroke.gradient_id =
        src->stroke.gradient_id
            ? svg__strndup(src->stroke.gradient_id, strlen(src->stroke.gradient_id))
            : nullptr;
    if (src->stroke.gradient_id && !dst->stroke.gradient_id) {
        svg__free(dst->fill.gradient_id);
        dst->fill.gradient_id = nullptr;
        return false;
    }
    return true;
}

static void style_dispose(svg_style *style) {
    free_paint(&style->fill);
    free_paint(&style->stroke);
}

/* ---- Shape emission ------------------------------------------------ */

bool svg__emit_shape(svg_parser *p, const svg_raw_shape *shape) {
    if (shape->cmd_count == 0)
        return true;
    if (p->shape_count >= p->limits.max_shapes)
        return false;
    if (p->shape_count == p->shape_cap) {
        uint32_t cap = p->shape_cap ? p->shape_cap * 2u : 32u;
        if (cap > p->limits.max_shapes)
            cap = p->limits.max_shapes;
        svg_raw_shape *grown =
            (svg_raw_shape *)svg__realloc(p->shapes, (size_t)cap * sizeof *grown);
        if (!grown)
            return false;
        p->shapes = grown;
        p->shape_cap = cap;
    }
    p->shapes[p->shape_count] = *shape;
    p->shape_count++;
    return true;
}

/* Build a shape from the accumulated command range and the current style. */
static bool finish_shape(svg_parser *p, uint32_t cmd_first, uint32_t cmd_count, flux_rect bounds) {
    if (cmd_count == 0)
        return true;
    const svg_style *style = style_current(p);
    svg_raw_shape shape = {
        .cmd_first = cmd_first,
        .cmd_count = cmd_count,
        .stroke_width = style->stroke_width,
        .miter_limit = style->miter_limit,
        .cap = style->cap,
        .join = style->join,
        .fill_rule = style->fill_rule,
        .opacity = style->alpha,
        .bounds = bounds,
    };
    /* Fill: literal paint minus `fill-opacity`; currentColor resolves to the
     * element's `color`. */
    shape.fill = style->fill;
    shape.fill.gradient_id = nullptr;
    shape.fill.opacity *= style->fill_opacity;
    if (shape.fill.kind == SVG_PAINT_CURRENT_COLOR && style->has_color) {
        shape.fill.kind = SVG_PAINT_SOLID;
        shape.fill.r = style->color_r;
        shape.fill.g = style->color_g;
        shape.fill.b = style->color_b;
        shape.fill.a = style->color_a;
    }
    shape.stroke = style->stroke;
    shape.stroke.gradient_id = nullptr;
    shape.stroke.opacity *= style->stroke_opacity;
    if (shape.stroke.kind == SVG_PAINT_CURRENT_COLOR && style->has_color) {
        shape.stroke.kind = SVG_PAINT_SOLID;
        shape.stroke.r = style->color_r;
        shape.stroke.g = style->color_g;
        shape.stroke.b = style->color_b;
        shape.stroke.a = style->color_a;
    }
    bool ok = true;
    if (style->fill.gradient_id) {
        shape.fill.gradient_id =
            svg__strndup(style->fill.gradient_id, strlen(style->fill.gradient_id));
        ok = ok && shape.fill.gradient_id != nullptr;
    }
    if (ok && style->stroke.gradient_id) {
        shape.stroke.gradient_id =
            svg__strndup(style->stroke.gradient_id, strlen(style->stroke.gradient_id));
        ok = ok && shape.stroke.gradient_id != nullptr;
    }
    /* The gradient kind is resolved later, when the definition is known. The
     * stored shape takes ownership of both `gradient_id` strings: on success
     * emit_shape copies the struct, so freeing here would leave the document
     * with dangling pointers that svg_release frees a second time. */
    if (ok && svg__emit_shape(p, &shape))
        return true;
    free_paint(&shape.fill);
    free_paint(&shape.stroke);
    return false;
}

/* ---- Root attributes and document metadata ------------------------ */

static bool record_attr(svg_parser *p, const char *name, size_t name_len, const char *value,
                        size_t value_len) {
    if (p->attr_count == p->attr_cap) {
        uint32_t cap = p->attr_cap ? p->attr_cap * 2u : 8u;
        svg_raw_attr *grown = (svg_raw_attr *)svg__realloc(p->attrs, (size_t)cap * sizeof *grown);
        if (!grown)
            return false;
        p->attrs = grown;
        p->attr_cap = cap;
    }
    char *owned_name = svg__strndup(name, name_len);
    char *owned_value = svg__strndup(value, value_len);
    if (!owned_name || !owned_value) {
        svg__free(owned_name);
        svg__free(owned_value);
        return false;
    }
    p->attrs[p->attr_count++] = (svg_raw_attr){.name = owned_name, .value = owned_value};
    return true;
}

static void parse_viewbox(svg_parser *p, const char *value, size_t len) {
    float parts[4];
    const char *cursor = value;
    const char *end = value + len;
    for (uint32_t i = 0; i < 4; ++i) {
        while (cursor < end && (is_ws(*cursor) || *cursor == ','))
            cursor++;
        bool ok = false;
        parts[i] = svg__parse_number(cursor, (size_t)(end - cursor), &ok);
        if (!ok)
            return;
        while (cursor < end && !is_ws(*cursor) && *cursor != ',')
            cursor++;
    }
    if (parts[2] <= 0.0f || parts[3] <= 0.0f)
        return;
    p->viewport = (flux_rect){parts[0], parts[1], parts[2], parts[3]};
    p->viewport_from_viewbox = true;
    p->saw_root = true;
}

static void parse_root_geometry(svg_parser *p, const svg_tag *tag) {
    const char *value = nullptr;
    size_t len = 0;
    if (attr_span(tag, "viewBox", &value, &len)) {
        parse_viewbox(p, value, len);
        return;
    }
    float width = 0.0f, height = 0.0f;
    if (attr_span(tag, "width", &value, &len))
        (void)svg__parse_length(value, len, 0.0f, &width);
    if (attr_span(tag, "height", &value, &len))
        (void)svg__parse_length(value, len, 0.0f, &height);
    if (width > 0.0f && height > 0.0f) {
        p->viewport = (flux_rect){0.0f, 0.0f, width, height};
        p->saw_root = true;
    }
}

/* ---- Gradient collection ----------------------------------------- */

static bool begin_gradient(svg_parser *p, const svg_tag *tag, svg_gradient_kind kind) {
    if (p->gradient_count >= p->limits.max_gradients)
        return false;
    free_gradient(&p->gradient);
    p->gradient = (svg_raw_gradient){
        .kind = kind,
        .units = SVG__UNITS_OBJECT_BBOX,
        /* An absent gradientTransform is the identity, not the zero matrix: a
         * zero matrix collapses every endpoint onto the origin and silently
         * paints a flat colour. */
        .transform = svg__xform_identity(),
    };
    const char *value = nullptr;
    size_t len = 0;
    if (attr_span(tag, "id", &value, &len)) {
        p->gradient.id = svg__strndup(value, len);
        if (!p->gradient.id)
            return false;
    }
    if (attr_span(tag, "href", &value, &len) || attr_span(tag, "xlink:href", &value, &len)) {
        if (len > 0 && value[0] == '#') {
            p->gradient.href = svg__strndup(value + 1, len - 1);
            if (!p->gradient.href)
                return false;
        }
    }
    if (attr_span(tag, "gradientUnits", &value, &len)) {
        p->gradient.units =
            span_eq(value, len, "userSpaceOnUse") ? SVG__UNITS_USER_SPACE : SVG__UNITS_OBJECT_BBOX;
        p->gradient.present |= SVG__FIELD_UNITS;
    }
    /* Defaults per spec, only relevant when nothing in the href chain sets
     * them; `present` keeps the distinction. */
    p->gradient.x1 = 0.0f;
    p->gradient.y1 = 0.0f;
    p->gradient.x2 = 1.0f;
    p->gradient.y2 = 0.0f;
    p->gradient.cx = 0.5f;
    p->gradient.cy = 0.5f;
    p->gradient.r = 0.5f;
    if (attr_span(tag, "x1", &value, &len) && svg__parse_length(value, len, 0.0f, &p->gradient.x1))
        p->gradient.present |= SVG__FIELD_X1;
    if (attr_span(tag, "y1", &value, &len) && svg__parse_length(value, len, 0.0f, &p->gradient.y1))
        p->gradient.present |= SVG__FIELD_Y1;
    if (attr_span(tag, "x2", &value, &len) && svg__parse_length(value, len, 0.0f, &p->gradient.x2))
        p->gradient.present |= SVG__FIELD_X2;
    if (attr_span(tag, "y2", &value, &len) && svg__parse_length(value, len, 0.0f, &p->gradient.y2))
        p->gradient.present |= SVG__FIELD_Y2;
    if (attr_span(tag, "cx", &value, &len) && svg__parse_length(value, len, 0.0f, &p->gradient.cx))
        p->gradient.present |= SVG__FIELD_CX;
    if (attr_span(tag, "cy", &value, &len) && svg__parse_length(value, len, 0.0f, &p->gradient.cy))
        p->gradient.present |= SVG__FIELD_CY;
    if (attr_span(tag, "r", &value, &len) && svg__parse_length(value, len, 0.0f, &p->gradient.r))
        p->gradient.present |= SVG__FIELD_R;
    if (attr_span(tag, "gradientTransform", &value, &len) &&
        svg__parse_transform(value, len, &p->gradient.transform))
        p->gradient.present |= SVG__FIELD_TRANSFORM;
    if (attr_span(tag, "spreadMethod", &value, &len) &&
        (span_eq(value, len, "reflect") || span_eq(value, len, "repeat")))
        p->gradient.spread_reflect_or_repeat = true;
    p->in_gradient = true;
    p->gradient_is_pending = true;
    return true;
}

static bool commit_gradient(svg_parser *p) {
    if (!p->gradient_is_pending)
        return true;
    p->gradient_is_pending = false;
    p->in_gradient = false;
    if (!p->gradient.id) {
        /* An unreferenced, unnamed gradient cannot be reached; drop it. */
        free_gradient(&p->gradient);
        return true;
    }
    if (p->gradient_count == p->gradient_cap) {
        uint32_t cap = p->gradient_cap ? p->gradient_cap * 2u : 8u;
        if (cap > p->limits.max_gradients)
            cap = p->limits.max_gradients;
        svg_raw_gradient *grown =
            (svg_raw_gradient *)svg__realloc(p->gradients, (size_t)cap * sizeof *grown);
        if (!grown)
            return false;
        p->gradients = grown;
        p->gradient_cap = cap;
    }
    p->gradients[p->gradient_count++] = p->gradient;
    p->gradient = (svg_raw_gradient){};
    return true;
}

static bool add_stop(svg_parser *p, const svg_tag *tag) {
    if (!p->in_gradient || !p->gradient_is_pending)
        return true;
    if (p->gradient.stop_count >= SVG__MAX_STOPS)
        return true;
    const char *value = nullptr;
    size_t len = 0;
    float offset = 0.0f;
    if (attr_span(tag, "offset", &value, &len)) {
        bool percent = len > 0 && value[len - 1] == '%';
        bool ok = false;
        float v = svg__parse_number(value, percent ? len - 1 : len, &ok);
        if (ok)
            offset = percent ? v / 100.0f : v;
        if (offset < 0.0f)
            offset = 0.0f;
        if (offset > 1.0f)
            offset = 1.0f;
    }
    uint8_t r = 0, g = 0, b = 0, a = 255;
    float opacity = 1.0f;
    if (attr_span(tag, "stop-color", &value, &len)) {
        if (!span_eq(value, len, "currentColor"))
            (void)svg__parse_color(value, len, &r, &g, &b, &a);
    }
    if (attr_span(tag, "stop-opacity", &value, &len))
        (void)parse_opacity(value, len, &opacity);
    /* style="" on a stop overrides the presentation attributes, exactly as it
     * does on a shape. CSS is a cascade, and a stop is not exempt from it. */
    if (attr_span(tag, "style", &value, &len)) {
        const char *cursor = value;
        const char *decl_end = value + len;
        while (cursor < decl_end) {
            const char *semi = cursor;
            while (semi < decl_end && *semi != ';')
                semi++;
            const char *colon = cursor;
            while (colon < semi && *colon != ':')
                colon++;
            if (colon < semi) {
                const char *name = cursor;
                const char *name_end = colon;
                while (name < name_end && is_ws(*name))
                    name++;
                while (name_end > name && is_ws(name_end[-1]))
                    name_end--;
                const char *val = colon + 1;
                while (val < semi && is_ws(*val))
                    val++;
                const char *val_end = semi;
                while (val_end > val && is_ws(val_end[-1]))
                    val_end--;
                const size_t name_len = (size_t)(name_end - name);
                const size_t val_len = (size_t)(val_end - val);
                if (name_eq(name, name_len, "stop-color") && !span_eq(val, val_len, "currentColor"))
                    (void)svg__parse_color(val, val_len, &r, &g, &b, &a);
                else if (name_eq(name, name_len, "stop-opacity"))
                    (void)parse_opacity(val, val_len, &opacity);
            }
            cursor = semi < decl_end ? semi + 1 : decl_end;
        }
    }
    p->gradient.stops[p->gradient.stop_count] = (svg_stop){
        .offset = offset,
        .r = r,
        .g = g,
        .b = b,
        .a = (uint8_t)lrintf(opacity * (float)a / 255.0f * 255.0f),
    };
    p->gradient.stop_count++;
    return true;
}

/* ---- Element geometry -------------------------------------------- */

/* Attribute lookup outcome. The three states are load-bearing: SVG's error
 * handling says a *present but invalid* geometry attribute disables the element,
 * while an *absent* one takes its specified default. Collapsing them (the usual
 * `bool` return) silently moves art to the origin. */
typedef enum attr_length_result {
    ATTR_LENGTH_ABSENT = 0,
    ATTR_LENGTH_OK = 1,
    ATTR_LENGTH_INVALID = 2,
} attr_length_result;

static attr_length_result attr_length(const svg_tag *tag, const char *name, float percent_ref,
                                      float *out) {
    const char *value = nullptr;
    size_t len = 0;
    if (!attr_span(tag, name, &value, &len))
        return ATTR_LENGTH_ABSENT;
    return svg__parse_length(value, len, percent_ref, out) ? ATTR_LENGTH_OK : ATTR_LENGTH_INVALID;
}

static bool parse_points(const char *value, size_t len, float **xy_out, uint32_t *count_out) {
    const char *cursor = value;
    const char *end = value + len;
    uint32_t cap = 16;
    uint32_t count = 0;
    float *xy = (float *)svg__alloc((size_t)cap * sizeof *xy);
    if (!xy)
        return false;
    while (true) {
        while (cursor < end && (is_ws(*cursor) || *cursor == ','))
            cursor++;
        if (cursor >= end)
            break;
        bool ok = false;
        float v = svg__parse_number(cursor, (size_t)(end - cursor), &ok);
        if (!ok)
            break;
        while (cursor < end && !is_ws(*cursor) && *cursor != ',')
            cursor++;
        if (count == cap) {
            cap *= 2u;
            float *grown = (float *)svg__realloc(xy, (size_t)cap * sizeof *grown);
            if (!grown) {
                svg__free(xy);
                return false;
            }
            xy = grown;
        }
        xy[count++] = v;
    }
    *xy_out = xy;
    *count_out = count / 2u; /* an odd trailing coordinate is dropped */
    return true;
}

static bool emit_element_geometry(svg_parser *p, const svg_tag *tag, const char *name,
                                  size_t name_len) {
    const svg_style *style = style_current(p);
    const float vw = p->viewport.w > 0.0f ? p->viewport.w : 1.0f;
    const float vh = p->viewport.h > 0.0f ? p->viewport.h : 1.0f;
    const uint32_t first = p->cmds.count;
    flux_rect bounds = {0.0f, 0.0f, 0.0f, 0.0f};
    bool ok = true;

    if (name_eq(name, name_len, "path")) {
        const char *value = nullptr;
        size_t len = 0;
        if (attr_span(tag, "d", &value, &len)) {
            /* An empty or whitespace-only `d` is legal SVG for "paints nothing" —
             * not a malformed document. Refusing it would fail a whole icon set
             * because one shape was left blank. */
            const char *probe = value;
            const char *probe_end = value + len;
            while (probe < probe_end && is_ws(*probe))
                probe++;
            ok = probe == probe_end ||
                 svg__parse_path_data(value, len, style->ctm, &p->cmds, &bounds);
        }
    } else if (name_eq(name, name_len, "rect")) {
        float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f, rx = 0.0f, ry = 0.0f;
        /* width/height are mandatory in SVG: an absent one means zero, which
         * disables the element; an invalid one disables it too. */
        if (attr_length(tag, "x", vw, &x) == ATTR_LENGTH_INVALID ||
            attr_length(tag, "y", vh, &y) == ATTR_LENGTH_INVALID ||
            attr_length(tag, "width", vw, &w) == ATTR_LENGTH_INVALID ||
            attr_length(tag, "height", vh, &h) == ATTR_LENGTH_INVALID)
            return true;
        const attr_length_result rx_result = attr_length(tag, "rx", vw, &rx);
        const attr_length_result ry_result = attr_length(tag, "ry", vh, &ry);
        if (rx_result == ATTR_LENGTH_INVALID || ry_result == ATTR_LENGTH_INVALID)
            return true;
        /* One radius given means "the same on both axes". */
        if (rx_result == ATTR_LENGTH_OK && ry_result == ATTR_LENGTH_ABSENT)
            ry = rx;
        else if (ry_result == ATTR_LENGTH_OK && rx_result == ATTR_LENGTH_ABSENT)
            rx = ry;
        ok = svg__emit_rect(&p->cmds, style->ctm, x, y, w, h, rx, ry, &bounds);
    } else if (name_eq(name, name_len, "circle")) {
        float cx = 0.0f, cy = 0.0f, r = 0.0f;
        if (attr_length(tag, "cx", vw, &cx) == ATTR_LENGTH_INVALID ||
            attr_length(tag, "cy", vh, &cy) == ATTR_LENGTH_INVALID)
            return true;
        if (attr_length(tag, "r", vw, &r) != ATTR_LENGTH_OK)
            return true; /* `r` is mandatory; absent or invalid disables the circle */
        ok = svg__emit_ellipse(&p->cmds, style->ctm, cx, cy, r, r, &bounds);
    } else if (name_eq(name, name_len, "ellipse")) {
        float cx = 0.0f, cy = 0.0f, rx = 0.0f, ry = 0.0f;
        if (attr_length(tag, "cx", vw, &cx) == ATTR_LENGTH_INVALID ||
            attr_length(tag, "cy", vh, &cy) == ATTR_LENGTH_INVALID)
            return true;
        if (attr_length(tag, "rx", vw, &rx) != ATTR_LENGTH_OK ||
            attr_length(tag, "ry", vh, &ry) != ATTR_LENGTH_OK)
            return true;
        ok = svg__emit_ellipse(&p->cmds, style->ctm, cx, cy, rx, ry, &bounds);
    } else if (name_eq(name, name_len, "line")) {
        float x1 = 0.0f, y1 = 0.0f, x2 = 0.0f, y2 = 0.0f;
        if (attr_length(tag, "x1", vw, &x1) == ATTR_LENGTH_INVALID ||
            attr_length(tag, "y1", vh, &y1) == ATTR_LENGTH_INVALID ||
            attr_length(tag, "x2", vw, &x2) == ATTR_LENGTH_INVALID ||
            attr_length(tag, "y2", vh, &y2) == ATTR_LENGTH_INVALID)
            return true;
        ok = svg__emit_line(&p->cmds, style->ctm, x1, y1, x2, y2, &bounds);
    } else if (name_eq(name, name_len, "polyline") || name_eq(name, name_len, "polygon")) {
        const char *value = nullptr;
        size_t len = 0;
        if (!attr_span(tag, "points", &value, &len))
            return true;
        float *xy = nullptr;
        uint32_t count = 0;
        if (!parse_points(value, len, &xy, &count))
            return false;
        ok = svg__emit_poly(&p->cmds, style->ctm, xy, count, name_eq(name, name_len, "polygon"),
                            &bounds);
        svg__free(xy);
    } else {
        return true; /* not a geometry element */
    }

    if (!ok) {
        /* Roll the command buffer back: a malformed shape must not leave half a
         * path behind for the next element to inherit. */
        p->cmds.count = first;
        return false;
    }
    /* A shape that produced no commands contributes no shape at all: an element
     * SVG disabled (zero extent, zero radius, empty path) is "not rendered", and
     * emitting an empty shape would make shape indices lie to a consumer. */
    if (p->cmds.count == first)
        return true;
    return finish_shape(p, first, p->cmds.count - first, bounds);
}

/* ---- Element dispatch -------------------------------------------- */

/* Elements whose children must not paint: either their content model is not
 * vector geometry (`<title>`, `<desc>`, `<style>`, `<script>`, `<metadata>`) or
 * their content is reached through indirection this library refuses
 * (`<use>`, `<clipPath>`, `<mask>`, `<pattern>`, `<marker>`, `<filter>`,
 * `<symbol>`, `<defs>`, `<text>`, `<image>`).
 *
 * Treating them as plain containers is the tempting shortcut and it is wrong:
 * a `<mask>`'s black rect would paint over the art it was meant to mask, and a
 * `<style>` sheet's markup would surface as shapes. Their subtrees are entered
 * in *skip mode*, where geometry is ignored but `<linearGradient>`,
 * `<radialGradient>` and `<stop>` are still collected — that is where a defs
 * block keeps its paint definitions. */
static bool is_opaque_container(const char *name, size_t len) {
    static const char *const CONTAINERS[] = {
        "defs", "symbol",  "title",  "desc",   "metadata", "style", "script", "clipPath",
        "mask", "pattern", "marker", "filter", "text",     "image", "use",
    };
    for (size_t i = 0; i < sizeof CONTAINERS / sizeof CONTAINERS[0]; ++i) {
        if (name_eq(name, len, CONTAINERS[i]))
            return true;
    }
    return false;
}

static bool is_gradient_element(const char *name, size_t len) {
    return name_eq(name, len, "linearGradient") || name_eq(name, len, "radialGradient");
}

static bool open_element(svg_parser *p, const svg_tag *tag) {
    const char *name = tag->name;
    const size_t name_len = tag->name_len;

    if (name_eq(name, name_len, "svg")) {
        if (!p->saw_root) {
            parse_root_geometry(p, tag);
            for (uint32_t i = 0; i < tag->attr_count; ++i) {
                const svg_attr *attr = &tag->attrs[i];
                /* Application metadata has no SVG vocabulary of its own, so it
                 * is carried on data-* attributes and read back verbatim. */
                if (attr->name_len > 5 && memcmp(attr->name, "data-", 5) == 0) {
                    if (!record_attr(p, attr->name, attr->name_len, attr->value, attr->value_len))
                        return false;
                }
            }
        }
        svg_style child = {};
        if (!style_clone(style_current(p), &child))
            return false;
        apply_tag_style(p, &child, tag);
        if (!style_push(p, &child)) {
            style_dispose(&child);
            return false;
        }
        return true;
    }

    if (is_gradient_element(name, name_len)) {
        return begin_gradient(p, tag,
                              name_eq(name, name_len, "linearGradient") ? SVG__GRADIENT_LINEAR
                                                                        : SVG__GRADIENT_RADIAL);
    }

    if (name_eq(name, name_len, "stop")) {
        return add_stop(p, tag);
    }

    if (is_opaque_container(name, name_len)) {
        if (!tag->self_closing)
            p->skip_depth++;
        return true;
    }

    if (p->skip_depth > 0) {
        /* Inside an opaque container: geometry and grouping are inert, and their
         * styles must not leak out. Only the gradient vocabulary above still
         * means something, because that is what a defs block defines. */
        return true;
    }

    if (name_eq(name, name_len, "g")) {
        svg_style child = {};
        if (!style_clone(style_current(p), &child))
            return false;
        /* Group opacity cascades multiplicatively into each descendant. */
        apply_tag_style(p, &child, tag);
        const char *value = nullptr;
        size_t len = 0;
        if (attr_span(tag, "transform", &value, &len)) {
            svg_transform transform;
            if (svg__parse_transform(value, len, &transform))
                child.ctm = svg__xform_mul(child.ctm, transform);
        }
        if (!style_push(p, &child)) {
            style_dispose(&child);
            return false;
        }
        return true;
    }

    if (name_eq(name, name_len, "path") || name_eq(name, name_len, "rect") ||
        name_eq(name, name_len, "circle") || name_eq(name, name_len, "ellipse") ||
        name_eq(name, name_len, "line") || name_eq(name, name_len, "polyline") ||
        name_eq(name, name_len, "polygon")) {
        /* A shape owns its own presentation attributes. Skipping this frame —
         * i.e. emitting geometry under the parent's style — silently drops every
         * per-element `fill`, `stroke`, `opacity` and `transform`, which is the
         * single most common way this kind of parser is wrong: the art renders,
         * so the picture looks plausible while the colours are the parent's.
         * Presentation attributes on the element are the near-universal style
         * idiom in cursor and icon sets, so this frame is not optional. */
        svg_style local = {};
        if (!style_clone(style_current(p), &local))
            return false;
        apply_tag_style(p, &local, tag);
        const char *value = nullptr;
        size_t len = 0;
        if (attr_span(tag, "transform", &value, &len)) {
            svg_transform transform;
            if (svg__parse_transform(value, len, &transform))
                local.ctm = svg__xform_mul(local.ctm, transform);
        }
        if (!style_push(p, &local)) {
            style_dispose(&local);
            return false;
        }
        /* The frame owns `local` now; `style_pop` disposes it. */
        bool ok = emit_element_geometry(p, tag, name, name_len);
        style_pop(p);
        return ok;
    }

    /* Unknown element: ignore it and its attributes. Forward compatibility is
     * the point of a tag scanner — an editor's private element must not be able
     * to break a whole document. */
    return true;
}

/* Does opening an element of this name push a style frame?
 *
 * The single source of truth for push/pop symmetry. `open_element` pushes for
 * `<svg>` unconditionally (the root seed and any nested `<svg>`), but for `<g>`
 * only outside an opaque container. A close that pops on a different rule than
 * the open pushed either swallows an *ancestor* frame (`<defs><g></g></defs>`
 * discarded the document's inherited fill) or leaves the stack unbalanced
 * (`<defs><svg></svg></defs>` would fail a document that is well-formed). */
static bool pushes_style_frame(const svg_parser *p, const char *name, size_t name_len) {
    if (name_eq(name, name_len, "svg"))
        return true;
    if (name_eq(name, name_len, "g"))
        return p->skip_depth == 0;
    return false;
}

static bool close_element(svg_parser *p, const svg_tag *tag) {
    if (name_eq(tag->name, tag->name_len, "linearGradient") ||
        name_eq(tag->name, tag->name_len, "radialGradient")) {
        return commit_gradient(p);
    }
    if (is_opaque_container(tag->name, tag->name_len)) {
        if (p->skip_depth > 0)
            p->skip_depth--;
        return true;
    }
    if (!pushes_style_frame(p, tag->name, tag->name_len))
        return true;
    /* An unmatched close means the markup is not a tree. Popping anyway would
     * eat an ancestor frame, so flag it and let the driver fail the parse: a
     * document whose nesting is broken cannot be rendered faithfully, and
     * "never a partial document" is the contract. */
    if (!style_pop(p))
        p->style_underflow = true;
    return true;
}

/* ------------------------------------------------------------------ */
/*  Driver                                                            */
/* ------------------------------------------------------------------ */

static void parser_deinit(svg_parser *p) {
    svg_cmd_buf_deinit(&p->cmds);
    for (uint32_t i = 0; i < p->shape_count; ++i) {
        free_paint(&p->shapes[i].fill);
        free_paint(&p->shapes[i].stroke);
    }
    svg__free(p->shapes);
    for (uint32_t i = 0; i < p->gradient_count; ++i)
        free_gradient(&p->gradients[i]);
    svg__free(p->gradients);
    free_gradient(&p->gradient);
    free_tag_attrs(p);
    svg__free(p->attrs);
    while (p->stack.depth > 0) {
        p->stack.depth--;
        style_dispose(&p->stack.frames[p->stack.depth]);
    }
}

static bool parse_into(svg_parser *p) {
    bool ok = true;
    while (p->p < p->end && ok) {
        const char *lt = (const char *)memchr(p->p, '<', (size_t)(p->end - p->p));
        if (!lt)
            break;
        p->p = lt;
        /* Comments, processing instructions, doctype and CDATA are skipped
         * wholesale; none of them can contribute geometry. */
        if (p->end - p->p >= 4 && memcmp(p->p, "<!--", 4) == 0) {
            const char *close = svg__find(p->p + 4, (size_t)(p->end - p->p - 4), "-->");
            if (!close)
                return false;
            p->p = close + 3;
            continue;
        }
        if (p->end - p->p >= 2 && memcmp(p->p, "<?", 2) == 0) {
            const char *close = svg__find(p->p + 2, (size_t)(p->end - p->p - 2), "?>");
            if (!close)
                return false;
            p->p = close + 2;
            continue;
        }
        if (p->end - p->p >= 9 && memcmp(p->p, "<![CDATA[", 9) == 0) {
            const char *close = svg__find(p->p + 9, (size_t)(p->end - p->p - 9), "]]>");
            if (!close)
                return false;
            p->p = close + 3;
            continue;
        }
        if (p->end - p->p >= 2 && memcmp(p->p, "<!", 2) == 0) {
            const char *close = (const char *)memchr(p->p + 2, '>', (size_t)(p->end - p->p - 2));
            if (!close)
                return false;
            p->p = close + 1;
            continue;
        }

        svg_tag tag = {};
        if (!read_tag(&p->p, p->end, &tag))
            return false;
        p->saw_markup = true;
        if (++p->element_count > p->limits.max_elements)
            return false;

        if (tag.is_end) {
            ok = close_element(p, &tag);
            continue;
        }

        /* Registered here rather than at the end so a document that only
         * carries a viewBox still reports it even if a later element fails. */
        ok = open_element(p, &tag);
        if (!ok)
            break;
        if (tag.self_closing) {
            if (name_eq(tag.name, tag.name_len, "linearGradient") ||
                name_eq(tag.name, tag.name_len, "radialGradient"))
                ok = commit_gradient(p);
            else if (pushes_style_frame(p, tag.name, tag.name_len)) {
                /* `open_element` pushed the frame, so this is the matching pop.
                 * Same predicate, so a `<g/>` inside a `<defs>` neither pushes
                 * nor pops, and a nested `<svg/>` does both. */
                if (!style_pop(p))
                    p->style_underflow = true;
            }
        }
        if (p->cmds.count > p->limits.max_commands)
            return false;
    }
    if (!ok)
        return false;
    if (p->gradient_is_pending && !commit_gradient(p))
        return false;
    /* Structural balance is not decoration: an unterminated document is exactly
     * the "truncated document" this library promises never to produce. A buffer
     * that ends mid-subtree means the caller fed a truncated file, and reporting
     * success would paint a fraction of the art with no indication why. The root
     * seed frame is depth 1, so a balanced document returns to 1 with no opaque
     * container left open. */
    if (p->stack.depth > 1 || p->skip_depth != 0 || p->style_underflow)
        return false;
    return true;
}

/* Geometric fallback viewport for a document with neither viewBox nor
 * width/height: the union of the shape bounds. An empty document reports 1x1 so
 * callers can divide by it unconditionally. */
static flux_rect derive_viewport(const svg_parser *p) {
    if (p->saw_root)
        return p->viewport;
    bool any = false;
    flux_rect union_rect = {0.0f, 0.0f, 0.0f, 0.0f};
    for (uint32_t i = 0; i < p->shape_count; ++i) {
        const flux_rect b = p->shapes[i].bounds;
        if (b.w <= 0.0f && b.h <= 0.0f)
            continue;
        if (!any) {
            union_rect = b;
            any = true;
            continue;
        }
        float left = b.x < union_rect.x ? b.x : union_rect.x;
        float top = b.y < union_rect.y ? b.y : union_rect.y;
        float right_a = b.x + b.w;
        float right_b = union_rect.x + union_rect.w;
        float bottom_a = b.y + b.h;
        float bottom_b = union_rect.y + union_rect.h;
        float right = right_a > right_b ? right_a : right_b;
        float bottom = bottom_a > bottom_b ? bottom_a : bottom_b;
        union_rect = (flux_rect){left, top, right - left, bottom - top};
    }
    if (!any)
        return (flux_rect){0.0f, 0.0f, 1.0f, 1.0f};
    if (union_rect.w <= 0.0f || union_rect.h <= 0.0f) {
        if (union_rect.w <= 0.0f)
            union_rect.w = 1.0f;
        if (union_rect.h <= 0.0f)
            union_rect.h = 1.0f;
    }
    return union_rect;
}

flux_result svg_parse(const svg_parse_opts *opts, svg_document **out) {
    if (!opts || !opts->text || !out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    *out = nullptr;

    // `SVG_LENGTH_MEASURE` is the only "measure it" request; every other value
    // is a real byte count, so zero of them is an empty input. Treating 0 as
    // "measure it" made an empty `(ptr, len)` pair read past the caller's
    // buffer (found by the svg_parse fuzz harness, ADR-0150).
    size_t length = opts->length;
    if (length == SVG_LENGTH_MEASURE)
        length = strlen(opts->text);
    if (length == 0 || length > SVG__MAX_INPUT_BYTES)
        return FLUX_ERROR_INVALID_ARGUMENT;

    svg_parser parser = {};
    parser.limits = (svg_parse_limits){
        .max_shapes = SVG__DEFAULT_MAX_SHAPES,
        .max_commands = SVG__DEFAULT_MAX_COMMANDS,
        .max_gradients = SVG__DEFAULT_MAX_GRADIENTS,
        .max_elements = SVG__DEFAULT_MAX_ELEMENTS,
        .max_depth = SVG__DEFAULT_MAX_DEPTH,
    };
    if (opts->limits) {
        if (opts->limits->max_shapes)
            parser.limits.max_shapes = opts->limits->max_shapes;
        if (opts->limits->max_commands)
            parser.limits.max_commands = opts->limits->max_commands;
        if (opts->limits->max_gradients)
            parser.limits.max_gradients = opts->limits->max_gradients;
        if (opts->limits->max_elements)
            parser.limits.max_elements = opts->limits->max_elements;
        if (opts->limits->max_depth)
            parser.limits.max_depth = opts->limits->max_depth;
    }
    parser.p = opts->text;
    parser.end = opts->text + length;
    /* Enforced inside the buffer, so a single element cannot overshoot the
     * ceiling before the parse loop gets a chance to notice. */
    parser.cmds.limit = parser.limits.max_commands;

    svg_style root = {};
    seed_root_style(&parser, &root);
    if (!style_push(&parser, &root)) {
        style_dispose(&root);
        return FLUX_ERROR_OUT_OF_MEMORY;
    }

    bool parsed = parse_into(&parser);
    /* Input with no markup at all is not a document; reporting success with zero
     * shapes would let a caller draw nothing and call it rendered. */
    if (parsed && !parser.saw_markup)
        parsed = false;
    if (!parsed) {
        parser_deinit(&parser);
        return FLUX_ERROR_INVALID_ARGUMENT;
    }

    svg_document *doc = (svg_document *)svg__alloc(sizeof *doc);
    if (!doc) {
        parser_deinit(&parser);
        return FLUX_ERROR_OUT_OF_MEMORY;
    }
    __atomic_add_fetch(&g_live_documents, 1u, __ATOMIC_RELAXED);
    *doc = (svg_document){
        .refs = 1,
        .viewport = derive_viewport(&parser),
        .cmds = parser.cmds.data,
        .cmd_count = parser.cmds.count,
        .raw_shapes = parser.shapes,
        .shape_count = parser.shape_count,
        .raw_gradients = parser.gradients,
        .raw_gradient_count = parser.gradient_count,
        .attrs = parser.attrs,
        .attr_count = parser.attr_count,
    };
    /* Ownership moved into the document; stop the parser from freeing it. */
    parser.cmds = (svg_cmd_buf){};
    parser.shapes = nullptr;
    parser.shape_count = 0;
    parser.shape_cap = 0;
    parser.gradients = nullptr;
    parser.gradient_count = 0;
    parser.gradient_cap = 0;
    parser.attrs = nullptr;
    parser.attr_count = 0;
    parser.attr_cap = 0;
    parser_deinit(&parser);

    if (!svg__resolve_paints(doc)) {
        svg_release(doc);
        return FLUX_ERROR_OUT_OF_MEMORY;
    }
    /* The raw gradient table is only needed for resolution. */
    for (uint32_t i = 0; i < doc->raw_gradient_count; ++i)
        free_gradient(&doc->raw_gradients[i]);
    svg__free(doc->raw_gradients);
    doc->raw_gradients = nullptr;
    doc->raw_gradient_count = 0;

    *out = doc;
    return FLUX_OK;
}

/* ------------------------------------------------------------------ */
/*  Document accessors                                                */
/* ------------------------------------------------------------------ */

svg_document *svg_retain(svg_document *doc) {
    if (doc) {
        uint32_t previous = __atomic_add_fetch(&doc->refs, 1u, __ATOMIC_RELAXED);
        /* Overflow would mean 4G live references; refusing is the only safe
         * answer, and it is unreachable in practice. */
        if (previous == 0)
            doc->refs = 1;
    }
    return doc;
}

void svg_release(svg_document *doc) {
    if (!doc)
        return;
    if (__atomic_sub_fetch(&doc->refs, 1u, __ATOMIC_ACQ_REL) != 0)
        return;
    for (uint32_t i = 0; i < doc->shape_count; ++i) {
        free_paint(&doc->raw_shapes[i].fill);
        free_paint(&doc->raw_shapes[i].stroke);
    }
    svg__free(doc->raw_shapes);
    svg__free(doc->shapes);
    svg__free(doc->cmds);
    svg__free(doc->gradients);
    for (uint32_t i = 0; i < doc->attr_count; ++i) {
        svg__free(doc->attrs[i].name);
        svg__free(doc->attrs[i].value);
    }
    svg__free(doc->attrs);
    svg__free(doc);
    __atomic_sub_fetch(&g_live_documents, 1u, __ATOMIC_RELAXED);
}

flux_rect svg_viewport(const svg_document *doc) {
    if (!doc)
        return (flux_rect){0.0f, 0.0f, 1.0f, 1.0f};
    return doc->viewport;
}

const char *svg_attribute(const svg_document *doc, const char *name) {
    if (!doc || !name)
        return nullptr;
    for (uint32_t i = 0; i < doc->attr_count; ++i) {
        if (strcmp(doc->attrs[i].name, name) == 0)
            return doc->attrs[i].value;
    }
    return nullptr;
}

uint32_t svg_shape_count(const svg_document *doc) {
    return doc ? doc->shape_count : 0;
}

flux_result svg_shape_at(const svg_document *doc, uint32_t index, svg_shape_view *out) {
    if (!doc || !out || index >= doc->shape_count)
        return FLUX_ERROR_OUT_OF_RANGE;
    *out = doc->shapes[index];
    return FLUX_OK;
}

flux_result svg_gradient_at(const svg_document *doc, uint32_t index, svg_gradient_view *out) {
    if (!doc || !out || index >= doc->gradient_count)
        return FLUX_ERROR_OUT_OF_RANGE;
    const svg_gradient *gradient = &doc->gradients[index];
    out->stop_count = gradient->stop_count;
    for (uint32_t i = 0; i < gradient->stop_count; ++i)
        out->stops[i] = gradient->stops[i];
    out->start = gradient->start;
    out->end = gradient->end;
    out->radius = gradient->radius;
    return FLUX_OK;
}

void svg_path_emit(const svg_shape_view *shape, flux_path *path) {
    if (!shape || !path)
        return;
    for (uint32_t i = 0; i < shape->cmd_count; ++i) {
        const svg_path_cmd *cmd = &shape->cmds[i];
        switch ((svg_path_verb)cmd->verb) {
        case SVG_PATH_MOVE:
            flux_path_move_to(path, cmd->params[0], cmd->params[1]);
            break;
        case SVG_PATH_LINE:
            flux_path_line_to(path, cmd->params[0], cmd->params[1]);
            break;
        case SVG_PATH_QUAD:
            flux_path_quad_to(path, cmd->params[0], cmd->params[1], cmd->params[2], cmd->params[3]);
            break;
        case SVG_PATH_CUBIC:
            flux_path_cubic_to(path, cmd->params[0], cmd->params[1], cmd->params[2], cmd->params[3],
                               cmd->params[4], cmd->params[5]);
            break;
        case SVG_PATH_CLOSE:
            flux_path_close(path);
            break;
        }
    }
}
