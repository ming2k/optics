/*
 * icon_svg.c — native C23 SVG icon path and style parser.
 *
 * Implements SVG 1.1 path and basic shape decomposition directly to Lens icon
 * commands (lens_icon_cmd) normalized into a 24x24 logical viewport.
 */

#include "icon_svg.h"

#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* -------------------------------------------------------------------------- */
/*  2D Affine Transform                                                       */
/* -------------------------------------------------------------------------- */

typedef struct svg_transform {
    float a, b, c, d, e, f;
} svg_transform;

static inline svg_transform svg_transform_identity(void) {
    return (svg_transform){1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
}

static inline svg_transform svg_transform_multiply(svg_transform t1, svg_transform t2) {
    return (svg_transform){
        .a = t1.a * t2.a + t1.c * t2.b,
        .b = t1.b * t2.a + t1.d * t2.b,
        .c = t1.a * t2.c + t1.c * t2.d,
        .d = t1.b * t2.c + t1.d * t2.d,
        .e = t1.a * t2.e + t1.c * t2.f + t1.e,
        .f = t1.b * t2.e + t1.d * t2.f + t1.f,
    };
}

static inline void svg_transform_point(svg_transform t, float x, float y, float *ox, float *oy) {
    *ox = t.a * x + t.c * y + t.e;
    *oy = t.b * x + t.d * y + t.f;
}

/* -------------------------------------------------------------------------- */
/*  Style State & Stack                                                       */
/* -------------------------------------------------------------------------- */

typedef enum svg_paint_type {
    SVG_PAINT_NONE = 0,
    SVG_PAINT_CURRENT_COLOR = 1,
    SVG_PAINT_COLOR = 2,
} svg_paint_type;

typedef struct svg_paint {
    svg_paint_type type;
    uint32_t color; /* 0xAARRGGBB */
} svg_paint;

typedef struct svg_style {
    svg_paint fill;
    svg_paint stroke;
    float stroke_width;
    float opacity;
    float fill_opacity;
    float stroke_opacity;
    bool visible;
    svg_transform transform;
} svg_style;

static inline svg_style svg_style_default(void) {
    return (svg_style){
        .fill = {.type = SVG_PAINT_NONE, .color = 0},
        .stroke = {.type = SVG_PAINT_NONE, .color = 0},
        .stroke_width = 1.0f,
        .opacity = 1.0f,
        .fill_opacity = 1.0f,
        .stroke_opacity = 1.0f,
        .visible = true,
        .transform = svg_transform_identity(),
    };
}

/* -------------------------------------------------------------------------- */
/*  Dynamic Buffers                                                           */
/* -------------------------------------------------------------------------- */

typedef struct svg_cmd_buf {
    lens_icon_cmd *data;
    uint32_t count;
    uint32_t cap;
} svg_cmd_buf;

static void cmd_buf_push(svg_cmd_buf *b, lens_icon_cmd cmd) {
    if (b->count == b->cap) {
        uint32_t ncap = b->cap ? b->cap * 2u : 64u;
        lens_icon_cmd *p = (lens_icon_cmd *)realloc(b->data, ncap * sizeof *p);
        if (!p)
            return;
        b->data = p;
        b->cap = ncap;
    }
    b->data[b->count++] = cmd;
}

typedef struct svg_shape_buf {
    lensi_svg_shape *data;
    uint32_t count;
    uint32_t cap;
} svg_shape_buf;

static void shape_buf_push(svg_shape_buf *b, lensi_svg_shape shape) {
    if (b->count == b->cap) {
        uint32_t ncap = b->cap ? b->cap * 2u : 16u;
        lensi_svg_shape *p = (lensi_svg_shape *)realloc(b->data, ncap * sizeof *p);
        if (!p)
            return;
        b->data = p;
        b->cap = ncap;
    }
    b->data[b->count++] = shape;
}

/* -------------------------------------------------------------------------- */
/*  Parser Context                                                            */
/* -------------------------------------------------------------------------- */

#define SVG_MAX_STYLE_DEPTH 32

typedef struct svg_parser {
    const char *p;
    svg_cmd_buf cmds;
    svg_shape_buf shapes;
    svg_style style_stack[SVG_MAX_STYLE_DEPTH];
    int style_depth;
    float vb_x, vb_y, vb_w, vb_h;
    bool has_vb;
    float svg_w, svg_h;
    bool has_w, has_h;
    bool has_fill_shapes;
    bool has_stroke_shapes;
} svg_parser;

static inline svg_style *curr_style(svg_parser *ctx) {
    return &ctx->style_stack[ctx->style_depth];
}

static void push_style(svg_parser *ctx) {
    if (ctx->style_depth + 1 < SVG_MAX_STYLE_DEPTH) {
        ctx->style_stack[ctx->style_depth + 1] = ctx->style_stack[ctx->style_depth];
        ctx->style_depth++;
    }
}

static void pop_style(svg_parser *ctx) {
    if (ctx->style_depth > 0)
        ctx->style_depth--;
}

/* -------------------------------------------------------------------------- */
/*  Lexer / Helpers                                                           */
/* -------------------------------------------------------------------------- */

static inline void skip_ws(const char **s) {
    while (**s && (isspace((unsigned char)**s) || **s == ','))
        (*s)++;
}

static bool parse_number(const char **s, float *out) {
    skip_ws(s);
    if (!**s)
        return false;
    char *end = nullptr;
    *out = strtof(*s, &end);
    if (end == *s)
        return false;
    *s = end;
    return true;
}

static bool parse_color(const char *str, svg_paint *out) {
    while (*str && isspace((unsigned char)*str))
        str++;
    if (!*str)
        return false;

    if (strncmp(str, "none", 4) == 0) {
        out->type = SVG_PAINT_NONE;
        out->color = 0;
        return true;
    }
    if (strncmp(str, "currentColor", 12) == 0) {
        out->type = SVG_PAINT_CURRENT_COLOR;
        out->color = 0;
        return true;
    }

    if (*str == '#') {
        str++;
        size_t len = 0;
        while (isxdigit((unsigned char)str[len]))
            len++;
        unsigned int r = 0, g = 0, b = 0, a = 255;
        if (len == 3) {
            unsigned int hex = (unsigned int)strtoul(str, nullptr, 16);
            r = ((hex >> 8) & 0xF) * 17;
            g = ((hex >> 4) & 0xF) * 17;
            b = (hex & 0xF) * 17;
        } else if (len == 6) {
            unsigned int hex = (unsigned int)strtoul(str, nullptr, 16);
            r = (hex >> 16) & 0xFF;
            g = (hex >> 8) & 0xFF;
            b = hex & 0xFF;
        } else if (len == 8) {
            unsigned int hex = (unsigned int)strtoul(str, nullptr, 16);
            r = (hex >> 24) & 0xFF;
            g = (hex >> 16) & 0xFF;
            b = (hex >> 8) & 0xFF;
            a = hex & 0xFF;
        } else {
            return false;
        }
        out->type = SVG_PAINT_COLOR;
        out->color = (a << 24) | (r << 16) | (g << 8) | b;
        return true;
    }

    /* Named colors */
    struct {
        const char *name;
        uint32_t color;
    } named[] = {
        {"black", 0xFF000000u},   {"white", 0xFFFFFFFFu},   {"red", 0xFFFF0000u},
        {"green", 0xFF008000u},   {"blue", 0xFF0000FFu},    {"yellow", 0xFFFFFF00u},
        {"cyan", 0xFF00FFFFu},    {"magenta", 0xFFFF00FFu}, {"gray", 0xFF808080u},
        {"grey", 0xFF808080u},    {"orange", 0xFFFFA500u},  {"purple", 0xFF800080u},
        {"transparent", 0x00000000u},
    };
    for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); ++i) {
        if (strncmp(str, named[i].name, strlen(named[i].name)) == 0) {
            out->type = (named[i].color == 0) ? SVG_PAINT_NONE : SVG_PAINT_COLOR;
            out->color = named[i].color;
            return true;
        }
    }

    return false;
}

static void parse_transform(const char *str, svg_transform *out) {
    svg_transform t = svg_transform_identity();
    const char *p = str;
    while (*p) {
        skip_ws(&p);
        if (!*p)
            break;
        if (strncmp(p, "matrix", 6) == 0) {
            p += 6;
            skip_ws(&p);
            if (*p == '(')
                p++;
            float a, b, c, d, e, f;
            if (parse_number(&p, &a) && parse_number(&p, &b) && parse_number(&p, &c) &&
                parse_number(&p, &d) && parse_number(&p, &e) && parse_number(&p, &f)) {
                t = svg_transform_multiply(t, (svg_transform){a, b, c, d, e, f});
            }
            while (*p && *p != ')')
                p++;
            if (*p == ')')
                p++;
        } else if (strncmp(p, "translate", 9) == 0) {
            p += 9;
            skip_ws(&p);
            if (*p == '(')
                p++;
            float tx = 0, ty = 0;
            if (parse_number(&p, &tx)) {
                parse_number(&p, &ty);
                t = svg_transform_multiply(t, (svg_transform){1, 0, 0, 1, tx, ty});
            }
            while (*p && *p != ')')
                p++;
            if (*p == ')')
                p++;
        } else if (strncmp(p, "scale", 5) == 0) {
            p += 5;
            skip_ws(&p);
            if (*p == '(')
                p++;
            float sx = 1, sy = 1;
            if (parse_number(&p, &sx)) {
                if (!parse_number(&p, &sy))
                    sy = sx;
                t = svg_transform_multiply(t, (svg_transform){sx, 0, 0, sy, 0, 0});
            }
            while (*p && *p != ')')
                p++;
            if (*p == ')')
                p++;
        } else if (strncmp(p, "rotate", 6) == 0) {
            p += 6;
            skip_ws(&p);
            if (*p == '(')
                p++;
            float angle = 0, cx = 0, cy = 0;
            if (parse_number(&p, &angle)) {
                float rad = angle * (float)M_PI / 180.0f;
                float cos_a = cosf(rad);
                float sin_a = sinf(rad);
                if (parse_number(&p, &cx) && parse_number(&p, &cy)) {
                    t = svg_transform_multiply(t, (svg_transform){1, 0, 0, 1, cx, cy});
                    t = svg_transform_multiply(t, (svg_transform){cos_a, sin_a, -sin_a, cos_a, 0, 0});
                    t = svg_transform_multiply(t, (svg_transform){1, 0, 0, 1, -cx, -cy});
                } else {
                    t = svg_transform_multiply(t, (svg_transform){cos_a, sin_a, -sin_a, cos_a, 0, 0});
                }
            }
            while (*p && *p != ')')
                p++;
            if (*p == ')')
                p++;
        } else {
            p++;
        }
    }
    *out = t;
}

/* -------------------------------------------------------------------------- */
/*  W3C Elliptical Arc to Cubic Beziers                                       */
/* -------------------------------------------------------------------------- */

static void emit_arc(svg_parser *ctx, svg_transform t, float x0, float y0, float rx, float ry,
                     float phi, int fa, int fs, float x1, float y1) {
    if (rx <= 0.0f || ry <= 0.0f || (x0 == x1 && y0 == y1)) {
        float px, py;
        svg_transform_point(t, x1, y1, &px, &py);
        cmd_buf_push(&ctx->cmds, (lens_icon_cmd){.type = 1, .params = {px, py}});
        return;
    }

    rx = fabsf(rx);
    ry = fabsf(ry);
    phi = phi * (float)M_PI / 180.0f;
    float cos_phi = cosf(phi);
    float sin_phi = sinf(phi);

    float dx = (x0 - x1) * 0.5f;
    float dy = (y0 - y1) * 0.5f;
    float x1p = cos_phi * dx + sin_phi * dy;
    float y1p = -sin_phi * dx + cos_phi * dy;

    float lambda = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
    if (lambda > 1.0f) {
        float sl = sqrtf(lambda);
        rx *= sl;
        ry *= sl;
    }

    float rx_sq = rx * rx;
    float ry_sq = ry * ry;
    float x1p_sq = x1p * x1p;
    float y1p_sq = y1p * y1p;

    float num = rx_sq * ry_sq - rx_sq * y1p_sq - ry_sq * x1p_sq;
    float den = rx_sq * y1p_sq + ry_sq * x1p_sq;
    float factor = (num <= 0.0f) ? 0.0f : sqrtf(num / den);
    if (fa == fs)
        factor = -factor;

    float cxp = factor * (rx * y1p / ry);
    float cyp = factor * (-ry * x1p / rx);

    float cx = cos_phi * cxp - sin_phi * cyp + (x0 + x1) * 0.5f;
    float cy = sin_phi * cxp + cos_phi * cyp + (y0 + y1) * 0.5f;

    float vx1 = (x1p - cxp) / rx;
    float vy1 = (y1p - cyp) / ry;
    float vx2 = (-x1p - cxp) / rx;
    float vy2 = (-y1p - cyp) / ry;

    float theta1 = atan2f(vy1, vx1);
    float dtheta = atan2f(vy2, vx2) - theta1;

    if (fs == 0 && dtheta > 0.0f)
        dtheta -= 2.0f * (float)M_PI;
    else if (fs == 1 && dtheta < 0.0f)
        dtheta += 2.0f * (float)M_PI;

    int n_segs = (int)ceilf(fabsf(dtheta) / ((float)M_PI * 0.5f));
    if (n_segs < 1)
        n_segs = 1;
    float seg_dt = dtheta / (float)n_segs;

    float th = theta1;
    for (int i = 0; i < n_segs; ++i) {
        float next_th = th + seg_dt;
        float half = seg_dt * 0.5f;
        float alpha = sinf(seg_dt) * (sqrtf(4.0f + 3.0f * tanf(half) * tanf(half)) - 1.0f) / 3.0f;

        float cos_th = cosf(th), sin_th = sinf(th);
        float cos_next = cosf(next_th), sin_next = sinf(next_th);

        float ep1_x = cx + rx * cos_th * cos_phi - ry * sin_th * sin_phi;
        float ep1_y = cy + rx * cos_th * sin_phi + ry * sin_th * cos_phi;

        float ep2_x = cx + rx * cos_next * cos_phi - ry * sin_next * sin_phi;
        float ep2_y = cy + rx * cos_next * sin_phi + ry * sin_next * cos_phi;

        float cp1_x = ep1_x - alpha * (-rx * sin_th * cos_phi - ry * cos_th * sin_phi);
        float cp1_y = ep1_y - alpha * (-rx * sin_th * sin_phi + ry * cos_th * cos_phi);

        float cp2_x = ep2_x + alpha * (-rx * sin_next * cos_phi - ry * cos_next * sin_phi);
        float cp2_y = ep2_y + alpha * (-rx * sin_next * sin_phi + ry * cos_next * cos_phi);

        float q1x, q1y, q2x, q2y, q3x, q3y;
        svg_transform_point(t, cp1_x, cp1_y, &q1x, &q1y);
        svg_transform_point(t, cp2_x, cp2_y, &q2x, &q2y);
        svg_transform_point(t, ep2_x, ep2_y, &q3x, &q3y);

        cmd_buf_push(&ctx->cmds, (lens_icon_cmd){
            .type = 2,
            .params = {q1x, q1y, q2x, q2y, q3x, q3y},
        });
        th = next_th;
    }
}

/* -------------------------------------------------------------------------- */
/*  W3C SVG Path 'd' Parser                                                   */
/* -------------------------------------------------------------------------- */

static void parse_path_d(svg_parser *ctx, const char *d, svg_transform t) {
    const char *p = d;
    float cur_x = 0.0f, cur_y = 0.0f;
    float start_x = 0.0f, start_y = 0.0f;
    float last_cpx = 0.0f, last_cpy = 0.0f;
    char last_cmd = 0;

    while (*p) {
        skip_ws(&p);
        if (!*p)
            break;
        char cmd = *p;
        if (isalpha((unsigned char)cmd)) {
            p++;
        } else if (last_cmd) {
            cmd = last_cmd;
            if (cmd == 'M')
                cmd = 'L';
            else if (cmd == 'm')
                cmd = 'l';
        } else {
            p++;
            continue;
        }

        switch (cmd) {
        case 'M':
        case 'm': {
            float x, y;
            if (parse_number(&p, &x) && parse_number(&p, &y)) {
                if (cmd == 'm') {
                    x += cur_x;
                    y += cur_y;
                }
                cur_x = start_x = x;
                cur_y = start_y = y;
                last_cpx = cur_x;
                last_cpy = cur_y;
                float tx, ty;
                svg_transform_point(t, cur_x, cur_y, &tx, &ty);
                cmd_buf_push(&ctx->cmds, (lens_icon_cmd){.type = 0, .params = {tx, ty}});
                last_cmd = cmd;
            }
            break;
        }
        case 'L':
        case 'l': {
            float x, y;
            while (parse_number(&p, &x) && parse_number(&p, &y)) {
                if (cmd == 'l') {
                    x += cur_x;
                    y += cur_y;
                }
                float x0 = cur_x, y0 = cur_y;
                cur_x = x;
                cur_y = y;
                last_cpx = cur_x;
                last_cpy = cur_y;
                float p0x, p0y, p3x, p3y;
                svg_transform_point(t, x0, y0, &p0x, &p0y);
                svg_transform_point(t, cur_x, cur_y, &p3x, &p3y);
                cmd_buf_push(&ctx->cmds, (lens_icon_cmd){
                    .type = 2,
                    .params = {p0x, p0y, p3x, p3y, p3x, p3y},
                });
            }
            last_cmd = cmd;
            break;
        }
        case 'H':
        case 'h': {
            float x;
            while (parse_number(&p, &x)) {
                if (cmd == 'h')
                    x += cur_x;
                float x0 = cur_x, y0 = cur_y;
                cur_x = x;
                last_cpx = cur_x;
                last_cpy = cur_y;
                float p0x, p0y, p3x, p3y;
                svg_transform_point(t, x0, y0, &p0x, &p0y);
                svg_transform_point(t, cur_x, cur_y, &p3x, &p3y);
                cmd_buf_push(&ctx->cmds, (lens_icon_cmd){
                    .type = 2,
                    .params = {p0x, p0y, p3x, p3y, p3x, p3y},
                });
            }
            last_cmd = cmd;
            break;
        }
        case 'V':
        case 'v': {
            float y;
            while (parse_number(&p, &y)) {
                if (cmd == 'v')
                    y += cur_y;
                float x0 = cur_x, y0 = cur_y;
                cur_y = y;
                last_cpx = cur_x;
                last_cpy = cur_y;
                float p0x, p0y, p3x, p3y;
                svg_transform_point(t, x0, y0, &p0x, &p0y);
                svg_transform_point(t, cur_x, cur_y, &p3x, &p3y);
                cmd_buf_push(&ctx->cmds, (lens_icon_cmd){
                    .type = 2,
                    .params = {p0x, p0y, p3x, p3y, p3x, p3y},
                });
            }
            last_cmd = cmd;
            break;
        }
        case 'C':
        case 'c': {
            float x1, y1, x2, y2, x, y;
            while (parse_number(&p, &x1) && parse_number(&p, &y1) && parse_number(&p, &x2) &&
                   parse_number(&p, &y2) && parse_number(&p, &x) && parse_number(&p, &y)) {
                if (cmd == 'c') {
                    x1 += cur_x;
                    y1 += cur_y;
                    x2 += cur_x;
                    y2 += cur_y;
                    x += cur_x;
                    y += cur_y;
                }
                float p1x, p1y, p2x, p2y, p3x, p3y;
                svg_transform_point(t, x1, y1, &p1x, &p1y);
                svg_transform_point(t, x2, y2, &p2x, &p2y);
                svg_transform_point(t, x, y, &p3x, &p3y);
                cmd_buf_push(&ctx->cmds,
                             (lens_icon_cmd){.type = 2, .params = {p1x, p1y, p2x, p2y, p3x, p3y}});
                last_cpx = x2;
                last_cpy = y2;
                cur_x = x;
                cur_y = y;
            }
            last_cmd = cmd;
            break;
        }
        case 'S':
        case 's': {
            float x2, y2, x, y;
            while (parse_number(&p, &x2) && parse_number(&p, &y2) && parse_number(&p, &x) &&
                   parse_number(&p, &y)) {
                if (cmd == 's') {
                    x2 += cur_x;
                    y2 += cur_y;
                    x += cur_x;
                    y += cur_y;
                }
                float x1 = (last_cmd == 'C' || last_cmd == 'c' || last_cmd == 'S' || last_cmd == 's')
                               ? 2.0f * cur_x - last_cpx
                               : cur_x;
                float y1 = (last_cmd == 'C' || last_cmd == 'c' || last_cmd == 'S' || last_cmd == 's')
                               ? 2.0f * cur_y - last_cpy
                               : cur_y;
                float p1x, p1y, p2x, p2y, p3x, p3y;
                svg_transform_point(t, x1, y1, &p1x, &p1y);
                svg_transform_point(t, x2, y2, &p2x, &p2y);
                svg_transform_point(t, x, y, &p3x, &p3y);
                cmd_buf_push(&ctx->cmds,
                             (lens_icon_cmd){.type = 2, .params = {p1x, p1y, p2x, p2y, p3x, p3y}});
                last_cpx = x2;
                last_cpy = y2;
                cur_x = x;
                cur_y = y;
                last_cmd = cmd;
            }
            break;
        }
        case 'Q':
        case 'q': {
            float x1, y1, x, y;
            while (parse_number(&p, &x1) && parse_number(&p, &y1) && parse_number(&p, &x) &&
                   parse_number(&p, &y)) {
                if (cmd == 'q') {
                    x1 += cur_x;
                    y1 += cur_y;
                    x += cur_x;
                    y += cur_y;
                }
                /* Convert Quadratic to Cubic */
                float cx1 = cur_x + 2.0f / 3.0f * (x1 - cur_x);
                float cy1 = cur_y + 2.0f / 3.0f * (y1 - cur_y);
                float cx2 = x + 2.0f / 3.0f * (x1 - x);
                float cy2 = y + 2.0f / 3.0f * (y1 - y);
                float p1x, p1y, p2x, p2y, p3x, p3y;
                svg_transform_point(t, cx1, cy1, &p1x, &p1y);
                svg_transform_point(t, cx2, cy2, &p2x, &p2y);
                svg_transform_point(t, x, y, &p3x, &p3y);
                cmd_buf_push(&ctx->cmds,
                             (lens_icon_cmd){.type = 2, .params = {p1x, p1y, p2x, p2y, p3x, p3y}});
                last_cpx = x1;
                last_cpy = y1;
                cur_x = x;
                cur_y = y;
            }
            last_cmd = cmd;
            break;
        }
        case 'T':
        case 't': {
            float x, y;
            while (parse_number(&p, &x) && parse_number(&p, &y)) {
                if (cmd == 't') {
                    x += cur_x;
                    y += cur_y;
                }
                float x1 = (last_cmd == 'Q' || last_cmd == 'q' || last_cmd == 'T' || last_cmd == 't')
                               ? 2.0f * cur_x - last_cpx
                               : cur_x;
                float y1 = (last_cmd == 'Q' || last_cmd == 'q' || last_cmd == 'T' || last_cmd == 't')
                               ? 2.0f * cur_y - last_cpy
                               : cur_y;
                float cx1 = cur_x + 2.0f / 3.0f * (x1 - cur_x);
                float cy1 = cur_y + 2.0f / 3.0f * (y1 - cur_y);
                float cx2 = x + 2.0f / 3.0f * (x1 - x);
                float cy2 = y + 2.0f / 3.0f * (y1 - y);
                float p1x, p1y, p2x, p2y, p3x, p3y;
                svg_transform_point(t, cx1, cy1, &p1x, &p1y);
                svg_transform_point(t, cx2, cy2, &p2x, &p2y);
                svg_transform_point(t, x, y, &p3x, &p3y);
                cmd_buf_push(&ctx->cmds,
                             (lens_icon_cmd){.type = 2, .params = {p1x, p1y, p2x, p2y, p3x, p3y}});
                last_cpx = x1;
                last_cpy = y1;
                cur_x = x;
                cur_y = y;
                last_cmd = cmd;
            }
            break;
        }
        case 'A':
        case 'a': {
            float rx, ry, phi, fa, fs, x, y;
            while (parse_number(&p, &rx) && parse_number(&p, &ry) && parse_number(&p, &phi) &&
                   parse_number(&p, &fa) && parse_number(&p, &fs) && parse_number(&p, &x) &&
                   parse_number(&p, &y)) {
                if (cmd == 'a') {
                    x += cur_x;
                    y += cur_y;
                }
                emit_arc(ctx, t, cur_x, cur_y, rx, ry, phi, (int)fa, (int)fs, x, y);
                cur_x = x;
                cur_y = y;
                last_cpx = cur_x;
                last_cpy = cur_y;
            }
            last_cmd = cmd;
            break;
        }
        case 'Z':
        case 'z': {
            cmd_buf_push(&ctx->cmds, (lens_icon_cmd){.type = 4});
            cur_x = start_x;
            cur_y = start_y;
            last_cpx = cur_x;
            last_cpy = cur_y;
            last_cmd = cmd;
            break;
        }
        default:
            p++;
            break;
        }
    }
}

/* -------------------------------------------------------------------------- */
/*  Shape Primitives                                                          */
/* -------------------------------------------------------------------------- */

static void emit_circle(svg_parser *ctx, float cx, float cy, float r, svg_transform t) {
    if (r <= 0.0f)
        return;
    const float k = 0.5522847498f * r;
    float p0x, p0y, p1x, p1y, p2x, p2y, p3x, p3y;

    /* Start at Right (cx + r, cy) */
    svg_transform_point(t, cx + r, cy, &p0x, &p0y);
    cmd_buf_push(&ctx->cmds, (lens_icon_cmd){.type = 0, .params = {p0x, p0y}});

    /* Right to Bottom */
    svg_transform_point(t, cx + r, cy + k, &p1x, &p1y);
    svg_transform_point(t, cx + k, cy + r, &p2x, &p2y);
    svg_transform_point(t, cx, cy + r, &p3x, &p3y);
    cmd_buf_push(&ctx->cmds,
                 (lens_icon_cmd){.type = 2, .params = {p1x, p1y, p2x, p2y, p3x, p3y}});

    /* Bottom to Left */
    svg_transform_point(t, cx - k, cy + r, &p1x, &p1y);
    svg_transform_point(t, cx - r, cy + k, &p2x, &p2y);
    svg_transform_point(t, cx - r, cy, &p3x, &p3y);
    cmd_buf_push(&ctx->cmds,
                 (lens_icon_cmd){.type = 2, .params = {p1x, p1y, p2x, p2y, p3x, p3y}});

    /* Left to Top */
    svg_transform_point(t, cx - r, cy - k, &p1x, &p1y);
    svg_transform_point(t, cx - k, cy - r, &p2x, &p2y);
    svg_transform_point(t, cx, cy - r, &p3x, &p3y);
    cmd_buf_push(&ctx->cmds,
                 (lens_icon_cmd){.type = 2, .params = {p1x, p1y, p2x, p2y, p3x, p3y}});

    /* Top to Right */
    svg_transform_point(t, cx + k, cy - r, &p1x, &p1y);
    svg_transform_point(t, cx + r, cy - k, &p2x, &p2y);
    svg_transform_point(t, cx + r, cy, &p3x, &p3y);
    cmd_buf_push(&ctx->cmds,
                 (lens_icon_cmd){.type = 2, .params = {p1x, p1y, p2x, p2y, p3x, p3y}});

    cmd_buf_push(&ctx->cmds, (lens_icon_cmd){.type = 4});
}

static void emit_rect(svg_parser *ctx, float x, float y, float w, float h, float rx, float ry,
                      svg_transform t) {
    if (w <= 0.0f || h <= 0.0f)
        return;
    if (rx < 0.0f)
        rx = 0.0f;
    if (ry < 0.0f)
        ry = 0.0f;
    if (rx == 0.0f && ry > 0.0f)
        rx = ry;
    if (ry == 0.0f && rx > 0.0f)
        ry = rx;
    if (rx > w * 0.5f)
        rx = w * 0.5f;
    if (ry > h * 0.5f)
        ry = h * 0.5f;

    if (rx <= 0.0f && ry <= 0.0f) {
        float p0x, p0y, p1x, p1y, p2x, p2y, p3x, p3y;
        svg_transform_point(t, x, y, &p0x, &p0y);
        svg_transform_point(t, x + w, y, &p1x, &p1y);
        svg_transform_point(t, x + w, y + h, &p2x, &p2y);
        svg_transform_point(t, x, y + h, &p3x, &p3y);
        cmd_buf_push(&ctx->cmds, (lens_icon_cmd){.type = 0, .params = {p0x, p0y}});
        cmd_buf_push(&ctx->cmds, (lens_icon_cmd){.type = 2, .params = {p0x, p0y, p1x, p1y, p1x, p1y}});
        cmd_buf_push(&ctx->cmds, (lens_icon_cmd){.type = 2, .params = {p1x, p1y, p2x, p2y, p2x, p2y}});
        cmd_buf_push(&ctx->cmds, (lens_icon_cmd){.type = 2, .params = {p2x, p2y, p3x, p3y, p3x, p3y}});
        cmd_buf_push(&ctx->cmds, (lens_icon_cmd){.type = 4});
    } else {
        /* Rounded rect with beziers */
        const float kx = 0.5522847498f * rx;
        const float ky = 0.5522847498f * ry;
        float p0x, p0y, p1x, p1y, p2x, p2y, p3x, p3y;

        svg_transform_point(t, x + rx, y, &p0x, &p0y);
        cmd_buf_push(&ctx->cmds, (lens_icon_cmd){.type = 0, .params = {p0x, p0y}});

        svg_transform_point(t, x + w - rx, y, &p1x, &p1y);
        cmd_buf_push(&ctx->cmds, (lens_icon_cmd){.type = 2, .params = {p0x, p0y, p1x, p1y, p1x, p1y}});

        svg_transform_point(t, x + w - rx + kx, y, &p1x, &p1y);
        svg_transform_point(t, x + w, y + ry - ky, &p2x, &p2y);
        svg_transform_point(t, x + w, y + ry, &p3x, &p3y);
        cmd_buf_push(&ctx->cmds,
                     (lens_icon_cmd){.type = 2, .params = {p1x, p1y, p2x, p2y, p3x, p3y}});

        svg_transform_point(t, x + w, y + h - ry, &p1x, &p1y);
        cmd_buf_push(&ctx->cmds, (lens_icon_cmd){.type = 2, .params = {p3x, p3y, p1x, p1y, p1x, p1y}});

        svg_transform_point(t, x + w, y + h - ry + ky, &p1x, &p1y);
        svg_transform_point(t, x + w - rx + kx, y + h, &p2x, &p2y);
        svg_transform_point(t, x + w - rx, y + h, &p3x, &p3y);
        cmd_buf_push(&ctx->cmds,
                     (lens_icon_cmd){.type = 2, .params = {p1x, p1y, p2x, p2y, p3x, p3y}});

        svg_transform_point(t, x + rx, y + h, &p1x, &p1y);
        cmd_buf_push(&ctx->cmds, (lens_icon_cmd){.type = 2, .params = {p3x, p3y, p1x, p1y, p1x, p1y}});

        svg_transform_point(t, x + rx - kx, y + h, &p1x, &p1y);
        svg_transform_point(t, x, y + h - ry + ky, &p2x, &p2y);
        svg_transform_point(t, x, y + h - ry, &p3x, &p3y);
        cmd_buf_push(&ctx->cmds,
                     (lens_icon_cmd){.type = 2, .params = {p1x, p1y, p2x, p2y, p3x, p3y}});

        svg_transform_point(t, x, y + ry, &p1x, &p1y);
        cmd_buf_push(&ctx->cmds, (lens_icon_cmd){.type = 2, .params = {p3x, p3y, p1x, p1y, p1x, p1y}});

        svg_transform_point(t, x, y + ry - ky, &p1x, &p1y);
        svg_transform_point(t, x + rx - kx, y, &p2x, &p2y);
        svg_transform_point(t, x + rx, y, &p3x, &p3y);
        cmd_buf_push(&ctx->cmds,
                     (lens_icon_cmd){.type = 2, .params = {p1x, p1y, p2x, p2y, p3x, p3y}});
        cmd_buf_push(&ctx->cmds, (lens_icon_cmd){.type = 4});
    }
}

static void emit_line(svg_parser *ctx, float x1, float y1, float x2, float y2, svg_transform t) {
    float p0x, p0y, p3x, p3y;
    svg_transform_point(t, x1, y1, &p0x, &p0y);
    svg_transform_point(t, x2, y2, &p3x, &p3y);
    cmd_buf_push(&ctx->cmds, (lens_icon_cmd){.type = 0, .params = {p0x, p0y}});
    cmd_buf_push(&ctx->cmds, (lens_icon_cmd){
        .type = 2,
        .params = {p0x, p0y, p3x, p3y, p3x, p3y},
    });
}

static void emit_polyline(svg_parser *ctx, const char *pts, bool closed, svg_transform t) {
    const char *p = pts;
    float x, y;
    bool first = true;
    float prev_x = 0, prev_y = 0;
    while (parse_number(&p, &x) && parse_number(&p, &y)) {
        float tx, ty;
        svg_transform_point(t, x, y, &tx, &ty);
        if (first) {
            cmd_buf_push(&ctx->cmds, (lens_icon_cmd){.type = 0, .params = {tx, ty}});
            first = false;
        } else {
            cmd_buf_push(&ctx->cmds, (lens_icon_cmd){
                .type = 2,
                .params = {prev_x, prev_y, tx, ty, tx, ty},
            });
        }
        prev_x = tx;
        prev_y = ty;
    }
    if (closed && !first) {
        cmd_buf_push(&ctx->cmds, (lens_icon_cmd){.type = 4});
    }
}

/* -------------------------------------------------------------------------- */
/*  Tag and Attribute Parsing                                                 */
/* -------------------------------------------------------------------------- */

static const char *find_attr(const char *tag, const char *name) {
    size_t nlen = strlen(name);
    const char *p = tag;
    while (*p && *p != '>') {
        if (isspace((unsigned char)*p)) {
            p++;
            while (*p && isspace((unsigned char)*p))
                p++;
            if (strncmp(p, name, nlen) == 0 && (p[nlen] == '=' || isspace((unsigned char)p[nlen]))) {
                p += nlen;
                while (*p && *p != '=')
                    p++;
                if (*p == '=') {
                    p++;
                    while (*p && isspace((unsigned char)*p))
                        p++;
                    if (*p == '"' || *p == '\'')
                        return p + 1;
                }
            }
        } else {
            p++;
        }
    }
    return nullptr;
}

static void get_attr_val(const char *attr_start, char *buf, size_t cap) {
    if (!attr_start || cap == 0) {
        if (cap > 0)
            buf[0] = '\0';
        return;
    }
    char quote = *(attr_start - 1);
    size_t i = 0;
    while (attr_start[i] && attr_start[i] != quote && i + 1 < cap) {
        buf[i] = attr_start[i];
        i++;
    }
    buf[i] = '\0';
}

static void parse_style_attr(const char *str, svg_style *st) {
    const char *p = str;
    while (*p) {
        skip_ws(&p);
        if (!*p)
            break;
        const char *semi = strchr(p, ';');
        size_t len = semi ? (size_t)(semi - p) : strlen(p);
        const char *colon = memchr(p, ':', len);
        if (colon) {
            char key[32] = {0};
            char val[64] = {0};
            size_t klen = (size_t)(colon - p);
            if (klen >= sizeof(key))
                klen = sizeof(key) - 1;
            memcpy(key, p, klen);
            key[klen] = '\0';

            const char *vstart = colon + 1;
            while (vstart < p + len && isspace((unsigned char)*vstart))
                vstart++;
            size_t vlen = (size_t)(p + len - vstart);
            while (vlen > 0 && isspace((unsigned char)vstart[vlen - 1]))
                vlen--;
            if (vlen >= sizeof(val))
                vlen = sizeof(val) - 1;
            memcpy(val, vstart, vlen);
            val[vlen] = '\0';

            if (strcmp(key, "fill") == 0)
                parse_color(val, &st->fill);
            else if (strcmp(key, "stroke") == 0)
                parse_color(val, &st->stroke);
            else if (strcmp(key, "stroke-width") == 0)
                st->stroke_width = strtof(val, nullptr);
            else if (strcmp(key, "opacity") == 0)
                st->opacity = strtof(val, nullptr);
            else if (strcmp(key, "fill-opacity") == 0)
                st->fill_opacity = strtof(val, nullptr);
            else if (strcmp(key, "stroke-opacity") == 0)
                st->stroke_opacity = strtof(val, nullptr);
            else if (strcmp(key, "display") == 0 && strcmp(val, "none") == 0)
                st->visible = false;
            else if (strcmp(key, "visibility") == 0 && strcmp(val, "hidden") == 0)
                st->visible = false;
        }
        p = semi ? semi + 1 : p + len;
    }
}

static void apply_element_style(const char *tag, svg_style *st) {
    char val[256];
    const char *attr;

    if ((attr = find_attr(tag, "style"))) {
        get_attr_val(attr, val, sizeof(val));
        parse_style_attr(val, st);
    }
    if ((attr = find_attr(tag, "fill"))) {
        get_attr_val(attr, val, sizeof(val));
        parse_color(val, &st->fill);
    }
    if ((attr = find_attr(tag, "stroke"))) {
        get_attr_val(attr, val, sizeof(val));
        parse_color(val, &st->stroke);
    }
    if ((attr = find_attr(tag, "stroke-width"))) {
        get_attr_val(attr, val, sizeof(val));
        st->stroke_width = strtof(val, nullptr);
    }
    if ((attr = find_attr(tag, "opacity"))) {
        get_attr_val(attr, val, sizeof(val));
        st->opacity = strtof(val, nullptr);
    }
    if ((attr = find_attr(tag, "fill-opacity"))) {
        get_attr_val(attr, val, sizeof(val));
        st->fill_opacity = strtof(val, nullptr);
    }
    if ((attr = find_attr(tag, "stroke-opacity"))) {
        get_attr_val(attr, val, sizeof(val));
        st->stroke_opacity = strtof(val, nullptr);
    }
    if ((attr = find_attr(tag, "transform"))) {
        get_attr_val(attr, val, sizeof(val));
        svg_transform elem_t = svg_transform_identity();
        parse_transform(val, &elem_t);
        st->transform = svg_transform_multiply(st->transform, elem_t);
    }
}

/* -------------------------------------------------------------------------- */
/*  Main Parser Loop                                                          */
/* -------------------------------------------------------------------------- */

static void parse_element(svg_parser *ctx, const char *elem, size_t elen) {
    if (strncmp(elem, "svg", 3) == 0 && (isspace((unsigned char)elem[3]) || elem[3] == '>')) {
        char val[128];
        const char *attr;
        if ((attr = find_attr(elem, "viewBox"))) {
            get_attr_val(attr, val, sizeof(val));
            const char *vp = val;
            if (parse_number(&vp, &ctx->vb_x) && parse_number(&vp, &ctx->vb_y) &&
                parse_number(&vp, &ctx->vb_w) && parse_number(&vp, &ctx->vb_h)) {
                ctx->has_vb = true;
            }
        }
        if ((attr = find_attr(elem, "width"))) {
            get_attr_val(attr, val, sizeof(val));
            ctx->svg_w = strtof(val, nullptr);
            if (ctx->svg_w > 0)
                ctx->has_w = true;
        }
        if ((attr = find_attr(elem, "height"))) {
            get_attr_val(attr, val, sizeof(val));
            ctx->svg_h = strtof(val, nullptr);
            if (ctx->svg_h > 0)
                ctx->has_h = true;
        }
        apply_element_style(elem, curr_style(ctx));
        return;
    }

    if (strncmp(elem, "g", 1) == 0 && (isspace((unsigned char)elem[1]) || elem[1] == '>')) {
        push_style(ctx);
        apply_element_style(elem, curr_style(ctx));
        return;
    }

    if (strncmp(elem, "/g", 2) == 0) {
        pop_style(ctx);
        return;
    }

    /* Shape Elements */
    svg_style st = *curr_style(ctx);
    apply_element_style(elem, &st);

    if (!st.visible)
        return;

    bool has_fill = st.fill.type != SVG_PAINT_NONE;
    bool has_stroke = st.stroke.type != SVG_PAINT_NONE;
    if (!has_fill && !has_stroke) {
        /* Default SVG fill is black if neither is specified, unless in an icon context */
        has_fill = true;
        st.fill.type = SVG_PAINT_CURRENT_COLOR;
    }

    uint32_t start_cmd = ctx->cmds.count;

    if (strncmp(elem, "path", 4) == 0) {
        const char *attr = find_attr(elem, "d");
        if (attr) {
            char *d = (char *)malloc(elen + 1);
            if (d) {
                get_attr_val(attr, d, elen + 1);
                parse_path_d(ctx, d, st.transform);
                free(d);
            }
        }
    } else if (strncmp(elem, "circle", 6) == 0) {
        char val[64];
        float cx = 0, cy = 0, r = 0;
        const char *attr;
        if ((attr = find_attr(elem, "cx"))) {
            get_attr_val(attr, val, sizeof(val));
            cx = strtof(val, nullptr);
        }
        if ((attr = find_attr(elem, "cy"))) {
            get_attr_val(attr, val, sizeof(val));
            cy = strtof(val, nullptr);
        }
        if ((attr = find_attr(elem, "r"))) {
            get_attr_val(attr, val, sizeof(val));
            r = strtof(val, nullptr);
        }
        emit_circle(ctx, cx, cy, r, st.transform);
    } else if (strncmp(elem, "rect", 4) == 0) {
        char val[64];
        float x = 0, y = 0, w = 0, h = 0, rx = 0, ry = 0;
        const char *attr;
        if ((attr = find_attr(elem, "x"))) {
            get_attr_val(attr, val, sizeof(val));
            x = strtof(val, nullptr);
        }
        if ((attr = find_attr(elem, "y"))) {
            get_attr_val(attr, val, sizeof(val));
            y = strtof(val, nullptr);
        }
        if ((attr = find_attr(elem, "width"))) {
            get_attr_val(attr, val, sizeof(val));
            w = strtof(val, nullptr);
        }
        if ((attr = find_attr(elem, "height"))) {
            get_attr_val(attr, val, sizeof(val));
            h = strtof(val, nullptr);
        }
        if ((attr = find_attr(elem, "rx"))) {
            get_attr_val(attr, val, sizeof(val));
            rx = strtof(val, nullptr);
        }
        if ((attr = find_attr(elem, "ry"))) {
            get_attr_val(attr, val, sizeof(val));
            ry = strtof(val, nullptr);
        }
        emit_rect(ctx, x, y, w, h, rx, ry, st.transform);
    } else if (strncmp(elem, "line", 4) == 0) {
        char val[64];
        float x1 = 0, y1 = 0, x2 = 0, y2 = 0;
        const char *attr;
        if ((attr = find_attr(elem, "x1"))) {
            get_attr_val(attr, val, sizeof(val));
            x1 = strtof(val, nullptr);
        }
        if ((attr = find_attr(elem, "y1"))) {
            get_attr_val(attr, val, sizeof(val));
            y1 = strtof(val, nullptr);
        }
        if ((attr = find_attr(elem, "x2"))) {
            get_attr_val(attr, val, sizeof(val));
            x2 = strtof(val, nullptr);
        }
        if ((attr = find_attr(elem, "y2"))) {
            get_attr_val(attr, val, sizeof(val));
            y2 = strtof(val, nullptr);
        }
        emit_line(ctx, x1, y1, x2, y2, st.transform);
    } else if (strncmp(elem, "polyline", 8) == 0 || strncmp(elem, "polygon", 7) == 0) {
        bool closed = (strncmp(elem, "polygon", 7) == 0);
        const char *attr = find_attr(elem, "points");
        if (attr) {
            char *pts = (char *)malloc(elen + 1);
            if (pts) {
                get_attr_val(attr, pts, elen + 1);
                emit_polyline(ctx, pts, closed, st.transform);
                free(pts);
            }
        }
    }

    uint32_t cmd_count = ctx->cmds.count - start_cmd;
    if (cmd_count > 0) {
        if (has_fill)
            ctx->has_fill_shapes = true;
        if (has_stroke)
            ctx->has_stroke_shapes = true;

        uint32_t raw_color = 0;
        if (has_fill && st.fill.type == SVG_PAINT_COLOR) {
            raw_color = st.fill.color;
            float total_opacity = st.opacity * st.fill_opacity;
            uint32_t a = (uint32_t)(((raw_color >> 24) & 0xFF) * total_opacity + 0.5f);
            if (a > 255)
                a = 255;
            raw_color = (a << 24) | (raw_color & 0x00FFFFFFu);
        } else if (has_stroke && st.stroke.type == SVG_PAINT_COLOR) {
            raw_color = st.stroke.color;
            float total_opacity = st.opacity * st.stroke_opacity;
            uint32_t a = (uint32_t)(((raw_color >> 24) & 0xFF) * total_opacity + 0.5f);
            if (a > 255)
                a = 255;
            raw_color = (a << 24) | (raw_color & 0x00FFFFFFu);
        }

        shape_buf_push(&ctx->shapes, (lensi_svg_shape){
                                         .color = raw_color,
                                         .filled = has_fill,
                                         .first_cmd = start_cmd,
                                         .cmd_count = cmd_count,
                                     });
    }
}

/* -------------------------------------------------------------------------- */
/*  Public Entry Point                                                        */
/* -------------------------------------------------------------------------- */

bool lensi_svg_parse(const char *svg_text, lensi_svg_result *out) {
    if (!svg_text || !out)
        return false;
    memset(out, 0, sizeof *out);

    svg_parser ctx = {
        .p = svg_text,
        .style_stack = {svg_style_default()},
        .style_depth = 0,
    };

    const char *p = svg_text;
    while (*p) {
        const char *tag_start = strchr(p, '<');
        if (!tag_start)
            break;
        if (tag_start[1] == '!' || tag_start[1] == '?') {
            /* Comment or XML declaration */
            const char *end = strstr(tag_start, tag_start[1] == '!' ? "-->" : "?>");
            p = end ? (tag_start[1] == '!' ? end + 3 : end + 2) : tag_start + 2;
            continue;
        }
        const char *tag_end = strchr(tag_start, '>');
        if (!tag_end)
            break;

        size_t len = (size_t)(tag_end - tag_start - 1);
        char *tag_buf = (char *)malloc(len + 1);
        if (tag_buf) {
            memcpy(tag_buf, tag_start + 1, len);
            tag_buf[len] = '\0';
            parse_element(&ctx, tag_buf, len);
            free(tag_buf);
        }
        p = tag_end + 1;
    }

    if (ctx.cmds.count == 0) {
        free(ctx.cmds.data);
        free(ctx.shapes.data);
        return false;
    }

    /* Compute ViewBox Bounds */
    float min_x = 0, min_y = 0, vb_w = 24.0f, vb_h = 24.0f;
    if (ctx.has_vb && ctx.vb_w > 0 && ctx.vb_h > 0) {
        min_x = ctx.vb_x;
        min_y = ctx.vb_y;
        vb_w = ctx.vb_w;
        vb_h = ctx.vb_h;
    } else if (ctx.has_w && ctx.has_h && ctx.svg_w > 0 && ctx.svg_h > 0) {
        min_x = 0;
        min_y = 0;
        vb_w = ctx.svg_w;
        vb_h = ctx.svg_h;
    } else {
        /* Compute bounds from geometry */
        float b_min_x = 1e9f, b_min_y = 1e9f, b_max_x = -1e9f, b_max_y = -1e9f;
        for (uint32_t i = 0; i < ctx.cmds.count; ++i) {
            lens_icon_cmd *c = &ctx.cmds.data[i];
            if (c->type == 0 || c->type == 1) {
                b_min_x = fminf(b_min_x, c->params[0]);
                b_min_y = fminf(b_min_y, c->params[1]);
                b_max_x = fmaxf(b_max_x, c->params[0]);
                b_max_y = fmaxf(b_max_y, c->params[1]);
            } else if (c->type == 2) {
                b_min_x = fminf(b_min_x, fminf(c->params[0], fminf(c->params[2], c->params[4])));
                b_min_y = fminf(b_min_y, fminf(c->params[1], fminf(c->params[3], c->params[5])));
                b_max_x = fmaxf(b_max_x, fmaxf(c->params[0], fmaxf(c->params[2], c->params[4])));
                b_max_y = fmaxf(b_max_y, fmaxf(c->params[1], fmaxf(c->params[3], c->params[5])));
            }
        }
        if (b_max_x > b_min_x && b_max_y > b_min_y) {
            min_x = b_min_x;
            min_y = b_min_y;
            vb_w = b_max_x - b_min_x;
            vb_h = b_max_y - b_min_y;
        }
    }

    /* Normalize to 24x24 Icon Box (Uniform scale with centering) */
    float scale = 24.0f / fmaxf(vb_w, vb_h);
    float ox = -min_x * scale + (24.0f - vb_w * scale) * 0.5f;
    float oy = -min_y * scale + (24.0f - vb_h * scale) * 0.5f;

    for (uint32_t i = 0; i < ctx.cmds.count; ++i) {
        lens_icon_cmd *c = &ctx.cmds.data[i];
        if (c->type == 0 || c->type == 1) {
            c->params[0] = c->params[0] * scale + ox;
            c->params[1] = c->params[1] * scale + oy;
        } else if (c->type == 2) {
            c->params[0] = c->params[0] * scale + ox;
            c->params[1] = c->params[1] * scale + oy;
            c->params[2] = c->params[2] * scale + ox;
            c->params[3] = c->params[3] * scale + oy;
            c->params[4] = c->params[4] * scale + ox;
            c->params[5] = c->params[5] * scale + oy;
        }
    }

    /* Build Color Runs (Merged by identical color + fill/stroke mode) */
    lens_icon_run *runs = nullptr;
    uint32_t run_count = 0;
    if (ctx.shapes.count > 0) {
        runs = (lens_icon_run *)malloc(ctx.shapes.count * sizeof *runs);
        if (runs) {
            for (uint32_t i = 0; i < ctx.shapes.count; ++i) {
                lensi_svg_shape s = ctx.shapes.data[i];
                if (run_count > 0 && runs[run_count - 1].color == s.color &&
                    runs[run_count - 1].fill == (uint8_t)s.filled &&
                    runs[run_count - 1].first_cmd + runs[run_count - 1].count == s.first_cmd) {
                    runs[run_count - 1].count += s.cmd_count;
                } else {
                    runs[run_count++] = (lens_icon_run){
                        .first_cmd = s.first_cmd,
                        .count = s.cmd_count,
                        .color = s.color,
                        .fill = (uint8_t)s.filled,
                    };
                }
            }

            /* If all runs are theme-colored (color == 0) and uniform, collapse to runs == nullptr */
            bool all_theme = true;
            for (uint32_t i = 0; i < run_count; ++i) {
                if (runs[i].color != 0) {
                    all_theme = false;
                    break;
                }
            }
            if (all_theme) {
                free(runs);
                runs = nullptr;
                run_count = 0;
            }
        }
    }

    free(ctx.shapes.data);

    out->cmds = ctx.cmds.data;
    out->cmd_count = ctx.cmds.count;
    out->runs = runs;
    out->run_count = run_count;
    out->has_fill = ctx.has_fill_shapes;
    out->has_stroke = ctx.has_stroke_shapes;

    return true;
}
