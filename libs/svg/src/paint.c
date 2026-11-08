/* paint.c — the value parsers (numbers, lengths, colours, transforms,
 * references) and the gradient resolver.
 *
 * Every parser here is total: it takes a pointer+length pair (never a
 * NUL-terminated string) and reports success by return value, so a malformed
 * attribute value degrades to "attribute absent" instead of aborting the parse.
 * That is the only behaviour an untrusted document may have. */

#include "internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- Names ------------------------------------------------------------ */

typedef struct svg_named_color {
    const char *name;
    uint8_t r, g, b;
} svg_named_color;

/* The CSS/SVG colour keywords. Hex and rgb()/rgba() cover generated art, but
 * hand-authored SVG uses names, and a name table that is nearly complete is
 * worse than none: it silently paints the wrong colour for the one keyword it
 * forgot. This is the whole list. */
static const svg_named_color NAMED_COLORS[] = {
    {"aliceblue", 240, 248, 255},
    {"antiquewhite", 250, 235, 215},
    {"aqua", 0, 255, 255},
    {"aquamarine", 127, 255, 212},
    {"azure", 240, 255, 255},
    {"beige", 245, 245, 220},
    {"bisque", 255, 228, 196},
    {"black", 0, 0, 0},
    {"blanchedalmond", 255, 235, 205},
    {"blue", 0, 0, 255},
    {"blueviolet", 138, 43, 226},
    {"brown", 165, 42, 42},
    {"burlywood", 222, 184, 135},
    {"cadetblue", 95, 158, 160},
    {"chartreuse", 127, 255, 0},
    {"chocolate", 210, 105, 30},
    {"coral", 255, 127, 80},
    {"cornflowerblue", 100, 149, 237},
    {"cornsilk", 255, 248, 220},
    {"crimson", 220, 20, 60},
    {"cyan", 0, 255, 255},
    {"darkblue", 0, 0, 139},
    {"darkcyan", 0, 139, 139},
    {"darkgoldenrod", 184, 134, 11},
    {"darkgray", 169, 169, 169},
    {"darkgreen", 0, 100, 0},
    {"darkgrey", 169, 169, 169},
    {"darkkhaki", 189, 183, 107},
    {"darkmagenta", 139, 0, 139},
    {"darkolivegreen", 85, 107, 47},
    {"darkorange", 255, 140, 0},
    {"darkorchid", 153, 50, 204},
    {"darkred", 139, 0, 0},
    {"darksalmon", 233, 150, 122},
    {"darkseagreen", 143, 188, 143},
    {"darkslateblue", 72, 61, 139},
    {"darkslategray", 47, 79, 79},
    {"darkslategrey", 47, 79, 79},
    {"darkturquoise", 0, 206, 209},
    {"darkviolet", 148, 0, 211},
    {"deeppink", 255, 20, 147},
    {"deepskyblue", 0, 191, 255},
    {"dimgray", 105, 105, 105},
    {"dimgrey", 105, 105, 105},
    {"dodgerblue", 30, 144, 255},
    {"firebrick", 178, 34, 34},
    {"floralwhite", 255, 250, 240},
    {"forestgreen", 34, 139, 34},
    {"fuchsia", 255, 0, 255},
    {"gainsboro", 220, 220, 220},
    {"ghostwhite", 248, 248, 255},
    {"gold", 255, 215, 0},
    {"goldenrod", 218, 165, 32},
    {"gray", 128, 128, 128},
    {"green", 0, 128, 0},
    {"greenyellow", 173, 255, 47},
    {"grey", 128, 128, 128},
    {"honeydew", 240, 255, 240},
    {"hotpink", 255, 105, 180},
    {"indianred", 205, 92, 92},
    {"indigo", 75, 0, 130},
    {"ivory", 255, 255, 240},
    {"khaki", 240, 230, 140},
    {"lavender", 230, 230, 250},
    {"lavenderblush", 255, 240, 245},
    {"lawngreen", 124, 252, 0},
    {"lemonchiffon", 255, 250, 205},
    {"lightblue", 173, 216, 230},
    {"lightcoral", 240, 128, 128},
    {"lightcyan", 224, 255, 255},
    {"lightgoldenrodyellow", 250, 250, 210},
    {"lightgray", 211, 211, 211},
    {"lightgreen", 144, 238, 144},
    {"lightgrey", 211, 211, 211},
    {"lightpink", 255, 182, 193},
    {"lightsalmon", 255, 160, 122},
    {"lightseagreen", 32, 178, 170},
    {"lightskyblue", 135, 206, 250},
    {"lightslategray", 119, 136, 153},
    {"lightslategrey", 119, 136, 153},
    {"lightsteelblue", 176, 196, 222},
    {"lightyellow", 255, 255, 224},
    {"lime", 0, 255, 0},
    {"limegreen", 50, 205, 50},
    {"linen", 250, 240, 230},
    {"magenta", 255, 0, 255},
    {"maroon", 128, 0, 0},
    {"mediumaquamarine", 102, 205, 170},
    {"mediumblue", 0, 0, 205},
    {"mediumorchid", 186, 85, 211},
    {"mediumpurple", 147, 112, 219},
    {"mediumseagreen", 60, 179, 113},
    {"mediumslateblue", 123, 104, 238},
    {"mediumspringgreen", 0, 250, 154},
    {"mediumturquoise", 72, 209, 204},
    {"mediumvioletred", 199, 21, 133},
    {"midnightblue", 25, 25, 112},
    {"mintcream", 245, 255, 250},
    {"mistyrose", 255, 228, 225},
    {"moccasin", 255, 228, 181},
    {"navajowhite", 255, 222, 173},
    {"navy", 0, 0, 128},
    {"oldlace", 253, 245, 230},
    {"olive", 128, 128, 0},
    {"olivedrab", 107, 142, 35},
    {"orange", 255, 165, 0},
    {"orangered", 255, 69, 0},
    {"orchid", 218, 112, 214},
    {"palegoldenrod", 238, 232, 170},
    {"palegreen", 152, 251, 152},
    {"paleturquoise", 175, 238, 238},
    {"palevioletred", 219, 112, 147},
    {"papayawhip", 255, 239, 213},
    {"peachpuff", 255, 218, 185},
    {"peru", 205, 133, 63},
    {"pink", 255, 192, 203},
    {"plum", 221, 160, 221},
    {"powderblue", 176, 224, 230},
    {"purple", 128, 0, 128},
    {"rebeccapurple", 102, 51, 153},
    {"red", 255, 0, 0},
    {"rosybrown", 188, 143, 143},
    {"royalblue", 65, 105, 225},
    {"saddlebrown", 139, 69, 19},
    {"salmon", 250, 128, 114},
    {"sandybrown", 244, 164, 96},
    {"seagreen", 46, 139, 87},
    {"seashell", 255, 245, 238},
    {"sienna", 160, 82, 45},
    {"silver", 192, 192, 192},
    {"skyblue", 135, 206, 235},
    {"slateblue", 106, 90, 205},
    {"slategray", 112, 128, 144},
    {"slategrey", 112, 128, 144},
    {"snow", 255, 250, 250},
    {"springgreen", 0, 255, 127},
    {"steelblue", 70, 130, 180},
    {"tan", 210, 180, 140},
    {"teal", 0, 128, 128},
    {"thistle", 216, 191, 216},
    {"tomato", 255, 99, 71},
    {"turquoise", 64, 224, 208},
    {"violet", 238, 130, 238},
    {"wheat", 245, 222, 179},
    {"white", 255, 255, 255},
    {"whitesmoke", 245, 245, 245},
    {"yellow", 255, 255, 0},
    {"yellowgreen", 154, 205, 50},
};

/* ---- Lexing helpers --------------------------------------------------- */

static bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static const char *skip_space(const char *p, const char *end) {
    while (p < end && is_space(*p))
        p++;
    return p;
}

/* A path/transform number: optional sign, digits, decimal point, exponent. */
static bool scan_number(const char **cursor, const char *end, float *out) {
    const char *p = skip_space(*cursor, end);
    const char *start = p;
    if (p < end && (*p == '+' || *p == '-'))
        p++;
    bool digits = false;
    while (p < end && *p >= '0' && *p <= '9') {
        p++;
        digits = true;
    }
    if (p < end && *p == '.') {
        p++;
        while (p < end && *p >= '0' && *p <= '9') {
            p++;
            digits = true;
        }
    }
    if (!digits)
        return false;
    if (p < end && (*p == 'e' || *p == 'E')) {
        const char *exp = p + 1;
        if (exp < end && (*exp == '+' || *exp == '-'))
            exp++;
        if (exp < end && *exp >= '0' && *exp <= '9') {
            p = exp;
            while (p < end && *p >= '0' && *p <= '9')
                p++;
        }
    }
    /* strtof needs a terminator; the span is short and already validated. */
    char tmp[64];
    size_t n = (size_t)(p - start);
    if (n == 0 || n >= sizeof tmp)
        return false;
    memcpy(tmp, start, n);
    tmp[n] = '\0';
    char *stop = nullptr;
    float v = strtof(tmp, &stop);
    if (stop == tmp)
        return false;
    *out = v;
    *cursor = p;
    return true;
}

float svg__parse_number(const char *value, size_t len, bool *ok) {
    const char *end = value + len;
    const char *cursor = value;
    float v = 0.0f;
    bool good = scan_number(&cursor, end, &v);
    if (ok)
        *ok = good;
    return good ? v : 0.0f;
}

bool svg__parse_length(const char *value, size_t len, float percent_ref, float *out) {
    const char *end = value + len;
    const char *cursor = value;
    float v = 0.0f;
    if (!scan_number(&cursor, end, &v))
        return false;
    cursor = skip_space(cursor, end);
    size_t unit_len = (size_t)(end - cursor);
    if (unit_len == 0) {
        *out = v;
        return true;
    }
    const char *unit = cursor;
    /* Absolute units are a pure scale on the user unit. */
    static const struct {
        const char *name;
        float scale;
    } ABSOLUTE[] = {
        {"px", 1.0f},          {"pt", 4.0f / 3.0f},   {"pc", 16.0f},
        {"mm", 96.0f / 25.4f}, {"cm", 96.0f / 2.54f}, {"in", 96.0f},
    };
    for (size_t i = 0; i < sizeof ABSOLUTE / sizeof ABSOLUTE[0]; ++i) {
        size_t n = strlen(ABSOLUTE[i].name);
        if (unit_len == n && memcmp(unit, ABSOLUTE[i].name, n) == 0) {
            *out = v * ABSOLUTE[i].scale;
            return true;
        }
    }
    if (unit_len == 1 && unit[0] == '%') {
        /* A percentage with no resolved reference axis is not a length this
         * library can honour; refusing beats inventing a denominator. */
        if (percent_ref <= 0.0f)
            return false;
        *out = v * percent_ref / 100.0f;
        return true;
    }
    /* em/ex/rem/vw/vh depend on a font or a viewport this library does not
     * own. Refuse rather than guess. */
    return false;
}

/* ---- Colour ----------------------------------------------------------- */

static int hex_digit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool svg__parse_color(const char *value, size_t len, uint8_t *r, uint8_t *g, uint8_t *b,
                      uint8_t *a) {
    const char *end = value + len;
    const char *p = skip_space(value, end);
    while (end > p && is_space(end[-1]))
        end--;
    if (p >= end)
        return false;

    if (*p == '#') {
        p++;
        size_t digits = (size_t)(end - p);
        int v[8];
        if (digits != 3 && digits != 4 && digits != 6 && digits != 8)
            return false;
        for (size_t i = 0; i < digits; ++i) {
            v[i] = hex_digit(p[i]);
            if (v[i] < 0)
                return false;
        }
        if (digits == 3 || digits == 4) {
            *r = (uint8_t)(v[0] * 17);
            *g = (uint8_t)(v[1] * 17);
            *b = (uint8_t)(v[2] * 17);
            *a = digits == 4 ? (uint8_t)(v[3] * 17) : 255u;
        } else {
            *r = (uint8_t)(v[0] * 16 + v[1]);
            *g = (uint8_t)(v[2] * 16 + v[3]);
            *b = (uint8_t)(v[4] * 16 + v[5]);
            *a = digits == 8 ? (uint8_t)(v[6] * 16 + v[7]) : 255u;
        }
        return true;
    }

    const size_t remaining = (size_t)(end - p);
    if (remaining > 4 && memcmp(p, "rgb(", 4) == 0) {
        const char *cursor = p + 4;
        float channels[3] = {0.0f, 0.0f, 0.0f};
        float alpha = 1.0f;
        for (uint32_t i = 0; i < 3; ++i) {
            cursor = skip_space(cursor, end);
            float v = 0.0f;
            if (!scan_number(&cursor, end, &v))
                return false;
            cursor = skip_space(cursor, end);
            if (cursor < end && *cursor == '%') {
                v = v * 255.0f / 100.0f;
                cursor++;
            }
            channels[i] = v;
            cursor = skip_space(cursor, end);
            if (i < 2) {
                if (cursor >= end || *cursor != ',')
                    return false;
                cursor++;
            }
        }
        cursor = skip_space(cursor, end);
        if (cursor < end && *cursor == '/') {
            cursor = skip_space(cursor + 1, end);
            if (!scan_number(&cursor, end, &alpha))
                return false;
        }
        uint8_t out[3];
        for (uint32_t i = 0; i < 3; ++i) {
            float v = channels[i];
            if (v < 0.0f)
                v = 0.0f;
            if (v > 255.0f)
                v = 255.0f;
            out[i] = (uint8_t)lrintf(v);
        }
        *r = out[0];
        *g = out[1];
        *b = out[2];
        *a = (uint8_t)lrintf(fminf(fmaxf(alpha, 0.0f), 1.0f) * 255.0f);
        return true;
    }

    if (remaining == 11 && memcmp(p, "transparent", 11) == 0) {
        *r = *g = *b = 0;
        *a = 0;
        return true;
    }
    if (remaining == 13 && memcmp(p, "currentcolor", 12) == 0)
        return false; /* handled by the caller as a paint kind, not a colour */

    for (size_t i = 0; i < sizeof NAMED_COLORS / sizeof NAMED_COLORS[0]; ++i) {
        size_t n = strlen(NAMED_COLORS[i].name);
        if (n != remaining)
            continue;
        bool same = true;
        for (size_t j = 0; j < n && same; ++j) {
            char c = p[j];
            if (c >= 'A' && c <= 'Z')
                c = (char)(c - 'A' + 'a');
            same = c == NAMED_COLORS[i].name[j];
        }
        if (same) {
            *r = NAMED_COLORS[i].r;
            *g = NAMED_COLORS[i].g;
            *b = NAMED_COLORS[i].b;
            *a = 255u;
            return true;
        }
    }
    return false;
}

/* ---- Transform -------------------------------------------------------- */

/* Consume `name(` at `*cursor`, or leave it alone. */
static bool expect_fn(const char **cursor, const char *end, const char *name) {
    const char *p = skip_space(*cursor, end);
    size_t n = strlen(name);
    if ((size_t)(end - p) < n || memcmp(p, name, n) != 0)
        return false;
    p += n;
    p = skip_space(p, end);
    if (p >= end || *p != '(')
        return false;
    *cursor = p + 1;
    return true;
}

static bool close_fn(const char **cursor, const char *end) {
    const char *p = skip_space(*cursor, end);
    if (p >= end || *p != ')')
        return false;
    *cursor = p + 1;
    return true;
}

bool svg__parse_transform(const char *value, size_t len, svg_transform *out) {
    const char *end = value + len;
    const char *cursor = value;
    svg_transform acc = svg__xform_identity();
    bool any = false;
    cursor = skip_space(cursor, end);
    while (cursor < end) {
        svg_transform step;
        float args[6];
        if (expect_fn(&cursor, end, "matrix")) {
            for (uint32_t argc = 0; argc < 6; ++argc)
                if (!scan_number(&cursor, end, &args[argc]))
                    return false;
            step = (svg_transform){args[0], args[1], args[2], args[3], args[4], args[5]};
        } else if (expect_fn(&cursor, end, "translate")) {
            if (!scan_number(&cursor, end, &args[0]))
                return false;
            args[1] = 0.0f;
            const char *probe = skip_space(cursor, end);
            if (probe < end && *probe != ')') {
                if (!scan_number(&cursor, end, &args[1]))
                    return false;
            }
            step = svg__xform_translate(args[0], args[1]);
        } else if (expect_fn(&cursor, end, "translateX")) {
            if (!scan_number(&cursor, end, &args[0]))
                return false;
            step = svg__xform_translate(args[0], 0.0f);
        } else if (expect_fn(&cursor, end, "translateY")) {
            if (!scan_number(&cursor, end, &args[0]))
                return false;
            step = svg__xform_translate(0.0f, args[0]);
        } else if (expect_fn(&cursor, end, "scale")) {
            if (!scan_number(&cursor, end, &args[0]))
                return false;
            args[1] = args[0];
            const char *probe = skip_space(cursor, end);
            if (probe < end && *probe != ')') {
                if (!scan_number(&cursor, end, &args[1]))
                    return false;
            }
            step = svg__xform_scale(args[0], args[1]);
        } else if (expect_fn(&cursor, end, "scaleX")) {
            if (!scan_number(&cursor, end, &args[0]))
                return false;
            step = svg__xform_scale(args[0], 1.0f);
        } else if (expect_fn(&cursor, end, "scaleY")) {
            if (!scan_number(&cursor, end, &args[0]))
                return false;
            step = svg__xform_scale(1.0f, args[0]);
        } else if (expect_fn(&cursor, end, "rotate")) {
            if (!scan_number(&cursor, end, &args[0]))
                return false;
            args[1] = 0.0f;
            args[2] = 0.0f;
            const char *probe = skip_space(cursor, end);
            const bool about_point = probe < end && *probe != ')';
            if (about_point &&
                (!scan_number(&cursor, end, &args[1]) || !scan_number(&cursor, end, &args[2])))
                return false;
            float radians = args[0] * 3.14159265358979323846f / 180.0f;
            float s = sinf(radians), c = cosf(radians);
            svg_transform rotate = (svg_transform){c, s, -s, c, 0.0f, 0.0f};
            if (about_point) {
                step =
                    svg__xform_mul(svg__xform_mul(svg__xform_translate(args[1], args[2]), rotate),
                                   svg__xform_translate(-args[1], -args[2]));
            } else {
                step = rotate;
            }
        } else if (expect_fn(&cursor, end, "skewX")) {
            if (!scan_number(&cursor, end, &args[0]))
                return false;
            step = (svg_transform){1.0f, 0.0f, tanf(args[0] * 3.14159265358979323846f / 180.0f),
                                   1.0f, 0.0f, 0.0f};
        } else if (expect_fn(&cursor, end, "skewY")) {
            if (!scan_number(&cursor, end, &args[0]))
                return false;
            step = (svg_transform){
                1.0f, tanf(args[0] * 3.14159265358979323846f / 180.0f), 0.0f, 1.0f, 0.0f, 0.0f};
        } else {
            /* Unknown transform function: refuse the whole attribute, so the
             * element paints untransformed rather than in the wrong place. */
            return false;
        }
        if (!close_fn(&cursor, end))
            return false;
        acc = svg__xform_mul(acc, step);
        any = true;
        cursor = skip_space(cursor, end);
        if (cursor < end && *cursor == ',')
            cursor = skip_space(cursor + 1, end);
    }
    if (!any)
        return false;
    *out = acc;
    return true;
}

/* ---- Paint references ------------------------------------------------- */

bool svg__parse_gradient_ref(const char *value, size_t len, char **id_out) {
    const char *end = value + len;
    const char *p = skip_space(value, end);
    while (end > p && is_space(end[-1]))
        end--;
    if ((size_t)(end - p) < 5 || memcmp(p, "url(", 4) != 0)
        return false;
    p += 4;
    p = skip_space(p, end);
    if (p >= end)
        return false;
    char quote = 0;
    if (*p == '"' || *p == '\'') {
        quote = *p;
        p++;
    }
    if (p >= end || *p != '#')
        return false;
    p++;
    const char *start = p;
    while (p < end && *p != ')' && !is_space(*p) && (!quote || *p != quote))
        p++;
    size_t n = (size_t)(p - start);
    if (n == 0)
        return false;
    *id_out = svg__strndup(start, n);
    return *id_out != nullptr;
}

/* ---- Gradient resolution ---------------------------------------------- */

static const svg_raw_gradient *find_raw_gradient(const svg_document *doc, const char *id) {
    if (!id)
        return nullptr;
    for (uint32_t i = 0; i < doc->raw_gradient_count; ++i) {
        const svg_raw_gradient *g = &doc->raw_gradients[i];
        if (g->id && strcmp(g->id, id) == 0)
            return g;
    }
    return nullptr;
}

/* href chains may be arbitrarily deep (in a hostile document, cyclic). The walk
 * is bounded by the table size, so a cycle terminates instead of hanging. */
static void find_stop_source(const svg_document *doc, const svg_raw_gradient *start,
                             const svg_stop **stop_source, uint32_t *stop_count) {
    const svg_raw_gradient *current = start;
    *stop_source = nullptr;
    *stop_count = 0;
    for (uint32_t depth = 0; depth <= doc->raw_gradient_count; ++depth) {
        if (current->stop_count) {
            *stop_source = current->stops;
            *stop_count = current->stop_count;
            return;
        }
        if (!current->href)
            return;
        const svg_raw_gradient *next = find_raw_gradient(doc, current->href);
        if (!next || next == start)
            return;
        current = next;
    }
}

/* Present-on-this-element wins; otherwise the first ancestor that wrote the
 * field. Geometry falls back to the SVG defaults only when nothing in the chain
 * specified it. */
static const svg_raw_gradient *find_field_owner(const svg_document *doc,
                                                const svg_raw_gradient *start, uint32_t field) {
    const svg_raw_gradient *current = start;
    for (uint32_t depth = 0; depth <= doc->raw_gradient_count; ++depth) {
        if (current->present & field)
            return current;
        if (!current->href)
            break;
        const svg_raw_gradient *next = find_raw_gradient(doc, current->href);
        if (!next || next == start)
            break;
        current = next;
    }
    return start;
}

static const svg_raw_gradient *find_units_owner(const svg_document *doc,
                                                const svg_raw_gradient *start) {
    return find_field_owner(doc, start, SVG__FIELD_UNITS);
}

bool svg__intern_gradient(svg_document *doc, const svg_gradient *gradient, uint32_t *index_out) {
    for (uint32_t i = 0; i < doc->gradient_count; ++i) {
        const svg_gradient *existing = &doc->gradients[i];
        if (existing->stop_count != gradient->stop_count)
            continue;
        if (existing->start.x != gradient->start.x || existing->start.y != gradient->start.y)
            continue;
        if (existing->end.x != gradient->end.x || existing->end.y != gradient->end.y)
            continue;
        if (existing->radius != gradient->radius)
            continue;
        bool same = true;
        for (uint32_t s = 0; s < gradient->stop_count && same; ++s) {
            same = existing->stops[s].t == gradient->stops[s].t &&
                   existing->stops[s].color == gradient->stops[s].color;
        }
        if (same) {
            *index_out = i;
            return true;
        }
    }
    svg_gradient *grown = (svg_gradient *)svg__realloc(
        doc->gradients, (size_t)(doc->gradient_count + 1u) * sizeof *grown);
    if (!grown)
        return false;
    doc->gradients = grown;
    doc->gradients[doc->gradient_count] = *gradient;
    *index_out = doc->gradient_count++;
    return true;
}

/* Build the flux-facing gradient for `raw` as referenced by a shape whose
 * user-space bounds are `bounds`. */
static bool build_gradient(const svg_document *doc, const svg_raw_gradient *raw, flux_rect bounds,
                           float paint_opacity, svg_gradient *out) {
    const svg_stop *stops = nullptr;
    uint32_t stop_count = 0;
    find_stop_source(doc, raw, &stops, &stop_count);
    if (stop_count == 0)
        return false; /* SVG: a gradient with no stops paints nothing */

    const svg_raw_gradient *units_owner = find_units_owner(doc, raw);
    const bool bbox = units_owner->units == SVG__UNITS_OBJECT_BBOX;
    const svg_raw_gradient *xform_owner = find_field_owner(doc, raw, SVG__FIELD_TRANSFORM);
    svg_transform xform = xform_owner->transform;

    /* In user space the geometry attributes are user units; in objectBoundingBox
     * space they are fractions of the shape's bounds. Baking the mapping into
     * `start`/`end`/`radius` is what lets flux paint a gradient without knowing
     * what an objectBoundingBox is. */
    float x1 = find_field_owner(doc, raw, SVG__FIELD_X1)->x1;
    float y1 = find_field_owner(doc, raw, SVG__FIELD_Y1)->y1;
    float x2 = find_field_owner(doc, raw, SVG__FIELD_X2)->x2;
    float y2 = find_field_owner(doc, raw, SVG__FIELD_Y2)->y2;
    float cx = find_field_owner(doc, raw, SVG__FIELD_CX)->cx;
    float cy = find_field_owner(doc, raw, SVG__FIELD_CY)->cy;
    float radius = find_field_owner(doc, raw, SVG__FIELD_R)->r;

    if (bbox) {
        /* A degenerate box makes objectBoundingBox meaningless; skip the
         * gradient exactly as the spec's "disable rendering" requires. */
        if (bounds.w <= 0.0f || bounds.h <= 0.0f)
            return false;
        x1 = bounds.x + x1 * bounds.w;
        x2 = bounds.x + x2 * bounds.w;
        cx = bounds.x + cx * bounds.w;
        y1 = bounds.y + y1 * bounds.h;
        y2 = bounds.y + y2 * bounds.h;
        cy = bounds.y + cy * bounds.h;
        /* flux radial gradients are circular; the objectBoundingBox mapping is
         * anisotropic. The normalised diagonal is the standard circular
         * approximation and preserves the gradient's visual extent. */
        radius = radius * 0.5f * sqrtf(bounds.w * bounds.w + bounds.h * bounds.h);
    }

    float p1x = 0.0f, p1y = 0.0f, p2x = 0.0f, p2y = 0.0f, centre_x = 0.0f, centre_y = 0.0f;
    svg__xform_point(xform, x1, y1, &p1x, &p1y);
    svg__xform_point(xform, x2, y2, &p2x, &p2y);
    svg__xform_point(xform, cx, cy, &centre_x, &centre_y);

    out->stop_count = 0;
    float previous_t = 0.0f;
    for (uint32_t i = 0; i < stop_count; ++i) {
        float t = stops[i].offset;
        if (t < 0.0f)
            t = 0.0f;
        if (t > 1.0f)
            t = 1.0f;
        if (t < previous_t)
            t = previous_t; /* offsets are non-decreasing by contract */
        previous_t = t;
        float alpha = (float)stops[i].a / 255.0f * paint_opacity;
        if (alpha < 0.0f)
            alpha = 0.0f;
        if (alpha > 1.0f)
            alpha = 1.0f;
        uint8_t a = (uint8_t)lrintf(alpha * 255.0f);
        out->stops[out->stop_count].t = t;
        out->stops[out->stop_count].color =
            flux_color_rgba_premul(stops[i].r, stops[i].g, stops[i].b, a);
        out->stop_count++;
    }
    /* flux needs at least two stops to interpolate; one stop means "flat
     * colour" in SVG, which a duplicated stop expresses exactly. */
    if (out->stop_count == 1) {
        out->stops[1] = out->stops[0];
        out->stops[1].t = 1.0f;
        out->stops[0].t = 0.0f;
        if (out->stops[1].t < out->stops[0].t)
            out->stops[1].t = out->stops[0].t;
        out->stop_count = 2;
    }

    if (raw->kind == SVG__GRADIENT_LINEAR) {
        out->start = (flux_point){p1x, p1y};
        out->end = (flux_point){p2x, p2y};
        out->radius = 0.0f;
        /* A zero-length linear gradient is the last stop painted flat. */
        if (fabsf(p2x - p1x) < 1e-6f && fabsf(p2y - p1y) < 1e-6f) {
            flux_color last = out->stops[out->stop_count - 1].color;
            out->stop_count = 2;
            out->stops[0] = (flux_gradient_stop){0.0f, last};
            out->stops[1] = (flux_gradient_stop){1.0f, last};
        }
    } else {
        out->start = (flux_point){centre_x, centre_y};
        out->end = (flux_point){centre_x, centre_y};
        out->radius = radius;
        if (radius <= 0.0f) {
            flux_color last = out->stops[out->stop_count - 1].color;
            out->stop_count = 2;
            out->stops[0] = (flux_gradient_stop){0.0f, last};
            out->stops[1] = (flux_gradient_stop){1.0f, last};
        }
    }
    return true;
}

/* Turn one raw paint into a resolved paint, interning any gradient it needs.
 * A paint that cannot be resolved degrades to SVG_PAINT_NONE: the shape keeps
 * its geometry (a caller may still tint it) but paints nothing by default, which
 * is what a dangling `url(#missing)` means in SVG anyway. */
static void resolve_paint(svg_document *doc, const svg_raw_paint *raw, flux_rect bounds,
                          svg_paint *out) {
    out->kind = raw->kind;
    out->color = 0;
    out->opacity = 1.0f;
    out->gradient = 0;
    switch (raw->kind) {
    case SVG_PAINT_NONE:
        return;
    case SVG_PAINT_CURRENT_COLOR:
        out->opacity = raw->opacity;
        return;
    case SVG_PAINT_SOLID:
        out->color = ((flux_color)raw->a << 24) | ((flux_color)raw->r << 16) |
                     ((flux_color)raw->g << 8) | (flux_color)raw->b;
        out->opacity = raw->opacity;
        return;
    case SVG_PAINT_LINEAR_GRADIENT:
    case SVG_PAINT_RADIAL_GRADIENT: {
        const svg_raw_gradient *raw_gradient = find_raw_gradient(doc, raw->gradient_id);
        if (!raw_gradient || raw->opacity <= 0.0f)
            break;
        /* The reference parser cannot know whether `url(#x)` names a linear or a
         * radial gradient; the definition decides, so the kind is corrected
         * here rather than guessed at paint time. */
        out->kind = raw_gradient->kind == SVG__GRADIENT_RADIAL ? SVG_PAINT_RADIAL_GRADIENT
                                                               : SVG_PAINT_LINEAR_GRADIENT;
        /* Gradient alpha already folds the paint opacity into every stop, so
         * the paint's own opacity is 1 here: a recolouring consumer must not be
         * able to apply it a second time. */
        svg_gradient gradient = {};
        if (!build_gradient(doc, raw_gradient, bounds, raw->opacity, &gradient))
            break;
        uint32_t index = 0;
        if (!svg__intern_gradient(doc, &gradient, &index))
            break;
        out->gradient = index;
        out->opacity = 1.0f;
        return;
    }
    }
    out->kind = SVG_PAINT_NONE;
    out->color = 0;
    out->opacity = 1.0f;
    out->gradient = 0;
}

bool svg__resolve_paints(svg_document *doc) {
    if (doc->shape_count == 0)
        return true;
    doc->shapes = (svg_shape_view *)svg__alloc((size_t)doc->shape_count * sizeof *doc->shapes);
    if (!doc->shapes)
        return false;
    for (uint32_t i = 0; i < doc->shape_count; ++i) {
        const svg_raw_shape *raw = &doc->raw_shapes[i];
        svg_shape_view *view = &doc->shapes[i];
        view->cmds = &doc->cmds[raw->cmd_first];
        view->cmd_count = raw->cmd_count;
        view->stroke_width = raw->stroke_width;
        view->miter_limit = raw->miter_limit;
        view->cap = raw->cap;
        view->join = raw->join;
        view->fill_rule = raw->fill_rule;
        view->opacity = raw->opacity;
        view->bounds = raw->bounds;
        resolve_paint(doc, &raw->fill, raw->bounds, &view->fill);
        resolve_paint(doc, &raw->stroke, raw->bounds, &view->stroke);
    }
    return true;
}
