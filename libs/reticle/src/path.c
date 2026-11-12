/* path.c — geometry: the SVG path grammar and the basic-shape decompositions.
 *
 * Everything richer than move/line/quad/cubic/close is decomposed here, once,
 * so no consumer re-implements SVG geometry. That includes elliptical arcs
 * (endpoint parameterisation → centre parameterisation → ≤90° cubic segments),
 * ellipses and rounded rects (circular-arc cubics with the standard kappa), and
 * the smooth-reflection rules for S/s and T/t.
 *
 * Every emitted point is transformed by the element's CTM as it is produced, so
 * the command stream is already in document user space and a consumer never
 * needs the matrix. Bounds are tracked in the same pass; they exclude stroke,
 * which is what `objectBoundingBox` gradient units require. */

#include "internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Circular arc → cubic approximation constant for a unit quarter arc. */
#define SVG_KAPPA 0.5522847498307936f

/* ---- Emit helpers ----------------------------------------------------- */

static bool emit(svg_cmd_buf *out, uint8_t verb, float a, float b, float c, float d, float e,
                 float f) {
    reticle_path_cmd cmd = {.verb = verb, .params = {a, b, c, d, e, f}};
    return svg_cmd_buf_push(out, cmd);
}

typedef struct bounds_tracker {
    flux_rect rect;
    bool any;
} bounds_tracker;

static void track(bounds_tracker *t, float x, float y) {
    if (!isfinite(x) || !isfinite(y))
        return;
    if (!t->any) {
        t->rect = (flux_rect){x, y, 0.0f, 0.0f};
        t->any = true;
        return;
    }
    float x1 = t->rect.x + t->rect.w;
    float y1 = t->rect.y + t->rect.h;
    float left = x < t->rect.x ? x : t->rect.x;
    float top = y < t->rect.y ? y : t->rect.y;
    float right = x > x1 ? x : x1;
    float bottom = y > y1 ? y : y1;
    t->rect = (flux_rect){left, top, right - left, bottom - top};
}

/* A cubic's true extremum, not its control-point hull: an objectBoundingBox
 * gradient sized to the hull is visibly wrong on a bowed path. */
static void track_cubic(bounds_tracker *t, float x0, float y0, float x1, float y1, float x2,
                        float y2, float x3, float y3) {
    track(t, x0, y0);
    track(t, x3, y3);
    const float px[4] = {x0, x1, x2, x3};
    const float py[4] = {y0, y1, y2, y3};
    for (uint32_t axis = 0; axis < 2; ++axis) {
        const float *p = axis ? py : px;
        /* B'(t) / 3 = a t^2 + b t + c */
        float a = -p[0] + 3.0f * p[1] - 3.0f * p[2] + p[3];
        float b = 2.0f * (p[0] - 2.0f * p[1] + p[2]);
        float c = p[1] - p[0];
        for (uint32_t root = 0; root < 2; ++root) {
            float t_solve = -1.0f;
            if (fabsf(a) < 1e-9f) {
                if (root == 0 && fabsf(b) > 1e-9f)
                    t_solve = -c / b;
            } else {
                float disc = b * b - 4.0f * a * c;
                if (disc < 0.0f)
                    continue;
                float sq = sqrtf(disc);
                t_solve = root == 0 ? (-b + sq) / (2.0f * a) : (-b - sq) / (2.0f * a);
            }
            if (!(t_solve > 0.0f && t_solve < 1.0f))
                continue;
            float mt = 1.0f - t_solve;
            float value = mt * mt * mt * p[0] + 3.0f * mt * mt * t_solve * p[1] +
                          3.0f * mt * t_solve * t_solve * p[2] + t_solve * t_solve * t_solve * p[3];
            if (axis)
                track(t, x0, value);
            else
                track(t, value, y0);
        }
    }
}

/* ---- Arc decomposition ------------------------------------------------ */

/* Endpoint → centre parameterisation, per SVG 1.1 implementation notes
 * F.6.5/F.6.6. Returns the number of ≤90° cubic segments emitted. */
static uint32_t emit_arc(svg_cmd_buf *out, svg_transform ctm, float x0, float y0, float rx,
                         float ry, float rotation_deg, bool large_arc, bool sweep, float x, float y,
                         bounds_tracker *tracker) {
    if (x0 == x && y0 == y)
        return 0;
    float rx_abs = fabsf(rx);
    float ry_abs = fabsf(ry);
    if (rx_abs < 1e-9f || ry_abs < 1e-9f) {
        float px = 0.0f, py = 0.0f;
        svg__xform_point(ctm, x, y, &px, &py);
        if (!emit(out, RETICLE_PATH_LINE, px, py, 0.0f, 0.0f, 0.0f, 0.0f))
            return 0;
        track(tracker, px, py);
        return 1;
    }

    float phi = rotation_deg * 3.14159265358979323846f / 180.0f;
    float cos_phi = cosf(phi), sin_phi = sinf(phi);

    float dx2 = (x0 - x) * 0.5f;
    float dy2 = (y0 - y) * 0.5f;
    float x1p = cos_phi * dx2 + sin_phi * dy2;
    float y1p = -sin_phi * dx2 + cos_phi * dy2;

    float lambda = (x1p * x1p) / (rx_abs * rx_abs) + (y1p * y1p) / (ry_abs * ry_abs);
    if (lambda > 1.0f) {
        float scale = sqrtf(lambda);
        rx_abs *= scale;
        ry_abs *= scale;
    }

    float rx2 = rx_abs * rx_abs;
    float ry2 = ry_abs * ry_abs;
    float numerator = rx2 * ry2 - rx2 * y1p * y1p - ry2 * x1p * x1p;
    float denominator = rx2 * y1p * y1p + ry2 * x1p * x1p;
    float factor = 0.0f;
    if (denominator > 0.0f)
        factor = sqrtf(fmaxf(numerator, 0.0f) / denominator);
    if (large_arc == sweep)
        factor = -factor;

    float cxp = factor * rx_abs * y1p / ry_abs;
    float cyp = -factor * ry_abs * x1p / rx_abs;

    float cx = cos_phi * cxp - sin_phi * cyp + (x0 + x) * 0.5f;
    float cy = sin_phi * cxp + cos_phi * cyp + (y0 + y) * 0.5f;

    float ux = (x1p - cxp) / rx_abs;
    float uy = (y1p - cyp) / ry_abs;
    float vx = (-x1p - cxp) / rx_abs;
    float vy = (-y1p - cyp) / ry_abs;

    float theta1 = atan2f(uy, ux);
    float delta = atan2f(ux * vy - uy * vx, ux * vx + uy * vy);
    if (!sweep && delta > 0.0f)
        delta -= 2.0f * 3.14159265358979323846f;
    else if (sweep && delta < 0.0f)
        delta += 2.0f * 3.14159265358979323846f;

    const float half_pi = 3.14159265358979323846f * 0.5f;
    /* Same tolerance as emit_elliptic_arc: an exact multiple of 90 degrees must
     * not round up to one extra segment on float error. A full sweep can only
     * ever need four, because the endpoint parameterisation caps |delta| at 2pi;
     * the clamp below is a spine for malformed input, not a policy. */
    uint32_t segments = (uint32_t)ceilf(fabsf(delta) / half_pi - 1e-4f);
    if (segments == 0)
        segments = 1;
    if (segments > 4)
        segments = 4;
    float step = delta / (float)segments;

    float prev_x = x0, prev_y = y0;
    for (uint32_t i = 0; i < segments; ++i) {
        float t1 = theta1 + step * (float)i;
        float t2 = t1 + step;
        float alpha = 4.0f / 3.0f * tanf(step * 0.25f);

        float cos_t2 = cosf(t2), sin_t2 = sinf(t2);
        float cos_a = cosf(t1), sin_a = sinf(t1);
        float cos_b = cos_t2, sin_b = sin_t2;

        /* Control and end points in the ellipse frame, then rotated and
         * translated into user space. */
        float c1x = cos_a - alpha * sin_a;
        float c1y = sin_a + alpha * cos_a;
        float c2x = cos_b + alpha * sin_b;
        float c2y = sin_b - alpha * cos_b;
        float ex = cos_b;
        float ey = sin_b;

        float u1 = rx_abs * c1x, v1 = ry_abs * c1y;
        float u2 = rx_abs * c2x, v2 = ry_abs * c2y;
        float u3 = rx_abs * ex, v3 = ry_abs * ey;

        float p1x = cos_phi * u1 - sin_phi * v1 + cx;
        float p1y = sin_phi * u1 + cos_phi * v1 + cy;
        float p2x = cos_phi * u2 - sin_phi * v2 + cx;
        float p2y = sin_phi * u2 + cos_phi * v2 + cy;
        float p3x = cos_phi * u3 - sin_phi * v3 + cx;
        float p3y = sin_phi * u3 + cos_phi * v3 + cy;

        float t1x = 0.0f, t1y = 0.0f, t2x = 0.0f, t2y = 0.0f, t3x = 0.0f, t3y = 0.0f;
        svg__xform_point(ctm, p1x, p1y, &t1x, &t1y);
        svg__xform_point(ctm, p2x, p2y, &t2x, &t2y);
        svg__xform_point(ctm, p3x, p3y, &t3x, &t3y);

        float start_x = 0.0f, start_y = 0.0f;
        svg__xform_point(ctm, prev_x, prev_y, &start_x, &start_y);
        if (!emit(out, RETICLE_PATH_CUBIC, t1x, t1y, t2x, t2y, t3x, t3y))
            return i;
        track_cubic(tracker, start_x, start_y, t1x, t1y, t2x, t2y, t3x, t3y);
        prev_x = p3x;
        prev_y = p3y;
    }
    return segments;
}

/* ---- Path data -------------------------------------------------------- */

typedef struct path_cursor {
    const char *p;
    const char *end;
} path_cursor;

static bool is_sep(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == ',';
}

static const char *skip_sep(const char *p, const char *end) {
    while (p < end && is_sep(*p))
        p++;
    return p;
}

static bool next_number(path_cursor *c, float *out) {
    c->p = skip_sep(c->p, c->end);
    const char *start = c->p;
    if (start >= c->end)
        return false;
    if (*start == '+' || *start == '-')
        start++;
    bool digits = false;
    const char *probe = start;
    while (probe < c->end && *probe >= '0' && *probe <= '9') {
        probe++;
        digits = true;
    }
    if (probe < c->end && *probe == '.') {
        probe++;
        while (probe < c->end && *probe >= '0' && *probe <= '9') {
            probe++;
            digits = true;
        }
    }
    if (!digits)
        return false;
    if (probe < c->end && (*probe == 'e' || *probe == 'E')) {
        const char *exp = probe + 1;
        if (exp < c->end && (*exp == '+' || *exp == '-'))
            exp++;
        if (exp < c->end && *exp >= '0' && *exp <= '9') {
            probe = exp;
            while (probe < c->end && *probe >= '0' && *probe <= '9')
                probe++;
        }
    }
    char tmp[64];
    size_t n = (size_t)(probe - c->p);
    if (n == 0 || n >= sizeof tmp)
        return false;
    memcpy(tmp, c->p, n);
    tmp[n] = '\0';
    char *stop = nullptr;
    float v = strtof(tmp, &stop);
    if (stop == tmp)
        return false;
    *out = v;
    c->p = probe;
    return true;
}

/* An arc flag is a single character, not a number: `a1 1 0 0 1 2 2` and
 * `a1 1 0 0012 2` are both legal, and the second only parses if flags are read
 * as characters. */
static bool next_flag(path_cursor *c, bool *out) {
    c->p = skip_sep(c->p, c->end);
    if (c->p >= c->end)
        return false;
    if (*c->p == '0' || *c->p == '1') {
        *out = *c->p == '1';
        c->p++;
        return true;
    }
    return false;
}

static bool next_command(path_cursor *c, char *out) {
    c->p = skip_sep(c->p, c->end);
    if (c->p >= c->end)
        return false;
    char ch = *c->p;
    bool is_cmd = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
    if (!is_cmd)
        return false;
    *out = ch;
    c->p++;
    return true;
}

bool svg__parse_path_data(const char *d, size_t len, svg_transform ctm, svg_cmd_buf *out,
                          flux_rect *bounds) {
    path_cursor c = {.p = d, .end = d + len};
    bounds_tracker tracker = {};
    /* Current point and the last control point, both in *pre-transform* user
     * space: the reflection rules for S/T are defined there. */
    float cx = 0.0f, cy = 0.0f;
    float start_x = 0.0f, start_y = 0.0f;
    float last_cubic_x = 0.0f, last_cubic_y = 0.0f;
    float last_quad_x = 0.0f, last_quad_y = 0.0f;
    char previous_verb = 0;
    char verb = 0;
    bool emitted = false;

    while (true) {
        c.p = skip_sep(c.p, c.end);
        if (c.p >= c.end)
            break;
        char next = 0;
        if (next_command(&c, &next)) {
            verb = next;
        } else if (verb == 0) {
            return false;
        } else if (verb == 'M') {
            verb = 'L'; /* implicit repetition of moveto is lineto */
        } else if (verb == 'm') {
            verb = 'l';
        } else if (verb == 'Z' || verb == 'z') {
            return false; /* a closepath takes no arguments, so nothing repeats */
        }

        switch (verb) {
        case 'M':
        case 'm': {
            float x = 0.0f, y = 0.0f;
            if (!next_number(&c, &x) || !next_number(&c, &y))
                return false;
            if (verb == 'm') {
                x += cx;
                y += cy;
            }
            cx = x;
            cy = y;
            start_x = x;
            start_y = y;
            float tx = 0.0f, ty = 0.0f;
            svg__xform_point(ctm, x, y, &tx, &ty);
            if (!emit(out, RETICLE_PATH_MOVE, tx, ty, 0.0f, 0.0f, 0.0f, 0.0f))
                return false;
            track(&tracker, tx, ty);
            emitted = true;
            previous_verb = verb;
            verb = (verb == 'm') ? 'l' : 'L';
            break;
        }
        case 'L':
        case 'l': {
            float x = 0.0f, y = 0.0f;
            if (!next_number(&c, &x) || !next_number(&c, &y))
                return false;
            if (verb == 'l') {
                x += cx;
                y += cy;
            }
            cx = x;
            cy = y;
            float tx = 0.0f, ty = 0.0f;
            svg__xform_point(ctm, x, y, &tx, &ty);
            if (!emit(out, RETICLE_PATH_LINE, tx, ty, 0.0f, 0.0f, 0.0f, 0.0f))
                return false;
            track(&tracker, tx, ty);
            emitted = true;
            previous_verb = verb;
            break;
        }
        case 'H':
        case 'h': {
            float x = 0.0f;
            if (!next_number(&c, &x))
                return false;
            if (verb == 'h')
                x += cx;
            cx = x;
            float tx = 0.0f, ty = 0.0f;
            svg__xform_point(ctm, cx, cy, &tx, &ty);
            if (!emit(out, RETICLE_PATH_LINE, tx, ty, 0.0f, 0.0f, 0.0f, 0.0f))
                return false;
            track(&tracker, tx, ty);
            emitted = true;
            previous_verb = verb;
            break;
        }
        case 'V':
        case 'v': {
            float y = 0.0f;
            if (!next_number(&c, &y))
                return false;
            if (verb == 'v')
                y += cy;
            cy = y;
            float tx = 0.0f, ty = 0.0f;
            svg__xform_point(ctm, cx, cy, &tx, &ty);
            if (!emit(out, RETICLE_PATH_LINE, tx, ty, 0.0f, 0.0f, 0.0f, 0.0f))
                return false;
            track(&tracker, tx, ty);
            emitted = true;
            previous_verb = verb;
            break;
        }
        case 'C':
        case 'c': {
            float x1 = 0.0f, y1 = 0.0f, x2 = 0.0f, y2 = 0.0f, x = 0.0f, y = 0.0f;
            if (!next_number(&c, &x1) || !next_number(&c, &y1) || !next_number(&c, &x2) ||
                !next_number(&c, &y2) || !next_number(&c, &x) || !next_number(&c, &y))
                return false;
            if (verb == 'c') {
                x1 += cx;
                y1 += cy;
                x2 += cx;
                y2 += cy;
                x += cx;
                y += cy;
            }
            float p0x = 0.0f, p0y = 0.0f, p1x = 0.0f, p1y = 0.0f;
            float p2x = 0.0f, p2y = 0.0f, p3x = 0.0f, p3y = 0.0f;
            svg__xform_point(ctm, cx, cy, &p0x, &p0y);
            svg__xform_point(ctm, x1, y1, &p1x, &p1y);
            svg__xform_point(ctm, x2, y2, &p2x, &p2y);
            svg__xform_point(ctm, x, y, &p3x, &p3y);
            if (!emit(out, RETICLE_PATH_CUBIC, p1x, p1y, p2x, p2y, p3x, p3y))
                return false;
            track_cubic(&tracker, p0x, p0y, p1x, p1y, p2x, p2y, p3x, p3y);
            last_cubic_x = x2;
            last_cubic_y = y2;
            cx = x;
            cy = y;
            emitted = true;
            previous_verb = verb;
            break;
        }
        case 'S':
        case 's': {
            float x2 = 0.0f, y2 = 0.0f, x = 0.0f, y = 0.0f;
            if (!next_number(&c, &x2) || !next_number(&c, &y2) || !next_number(&c, &x) ||
                !next_number(&c, &y))
                return false;
            if (verb == 's') {
                x2 += cx;
                y2 += cy;
                x += cx;
                y += cy;
            }
            bool reflect = previous_verb == 'C' || previous_verb == 'c' || previous_verb == 'S' ||
                           previous_verb == 's';
            float x1 = reflect ? 2.0f * cx - last_cubic_x : cx;
            float y1 = reflect ? 2.0f * cy - last_cubic_y : cy;
            float p0x = 0.0f, p0y = 0.0f, p1x = 0.0f, p1y = 0.0f;
            float p2x = 0.0f, p2y = 0.0f, p3x = 0.0f, p3y = 0.0f;
            svg__xform_point(ctm, cx, cy, &p0x, &p0y);
            svg__xform_point(ctm, x1, y1, &p1x, &p1y);
            svg__xform_point(ctm, x2, y2, &p2x, &p2y);
            svg__xform_point(ctm, x, y, &p3x, &p3y);
            if (!emit(out, RETICLE_PATH_CUBIC, p1x, p1y, p2x, p2y, p3x, p3y))
                return false;
            track_cubic(&tracker, p0x, p0y, p1x, p1y, p2x, p2y, p3x, p3y);
            last_cubic_x = x2;
            last_cubic_y = y2;
            cx = x;
            cy = y;
            emitted = true;
            previous_verb = verb;
            break;
        }
        case 'Q':
        case 'q': {
            float x1 = 0.0f, y1 = 0.0f, x = 0.0f, y = 0.0f;
            if (!next_number(&c, &x1) || !next_number(&c, &y1) || !next_number(&c, &x) ||
                !next_number(&c, &y))
                return false;
            if (verb == 'q') {
                x1 += cx;
                y1 += cy;
                x += cx;
                y += cy;
            }
            float p1x = 0.0f, p1y = 0.0f, p2x = 0.0f, p2y = 0.0f;
            svg__xform_point(ctm, x1, y1, &p1x, &p1y);
            svg__xform_point(ctm, x, y, &p2x, &p2y);
            if (!emit(out, RETICLE_PATH_QUAD, p1x, p1y, p2x, p2y, 0.0f, 0.0f))
                return false;
            /* Flatten the quad's hull for bounds: exact enough, and quads in
             * icon art are small. */
            float p0x = 0.0f, p0y = 0.0f;
            svg__xform_point(ctm, cx, cy, &p0x, &p0y);
            track(&tracker, p0x, p0y);
            track(&tracker, p1x, p1y);
            track(&tracker, p2x, p2y);
            last_quad_x = x1;
            last_quad_y = y1;
            cx = x;
            cy = y;
            emitted = true;
            previous_verb = verb;
            break;
        }
        case 'T':
        case 't': {
            float x = 0.0f, y = 0.0f;
            if (!next_number(&c, &x) || !next_number(&c, &y))
                return false;
            if (verb == 't') {
                x += cx;
                y += cy;
            }
            bool reflect = previous_verb == 'Q' || previous_verb == 'q' || previous_verb == 'T' ||
                           previous_verb == 't';
            float x1 = reflect ? 2.0f * cx - last_quad_x : cx;
            float y1 = reflect ? 2.0f * cy - last_quad_y : cy;
            float p1x = 0.0f, p1y = 0.0f, p2x = 0.0f, p2y = 0.0f;
            svg__xform_point(ctm, x1, y1, &p1x, &p1y);
            svg__xform_point(ctm, x, y, &p2x, &p2y);
            if (!emit(out, RETICLE_PATH_QUAD, p1x, p1y, p2x, p2y, 0.0f, 0.0f))
                return false;
            float p0x = 0.0f, p0y = 0.0f;
            svg__xform_point(ctm, cx, cy, &p0x, &p0y);
            track(&tracker, p0x, p0y);
            track(&tracker, p1x, p1y);
            track(&tracker, p2x, p2y);
            last_quad_x = x1;
            last_quad_y = y1;
            cx = x;
            cy = y;
            emitted = true;
            previous_verb = verb;
            break;
        }
        case 'A':
        case 'a': {
            float rx = 0.0f, ry = 0.0f, rotation = 0.0f, x = 0.0f, y = 0.0f;
            bool large = false, sweep = false;
            if (!next_number(&c, &rx) || !next_number(&c, &ry) || !next_number(&c, &rotation) ||
                !next_flag(&c, &large) || !next_flag(&c, &sweep) || !next_number(&c, &x) ||
                !next_number(&c, &y))
                return false;
            if (verb == 'a') {
                x += cx;
                y += cy;
            }
            emit_arc(out, ctm, cx, cy, rx, ry, rotation, large, sweep, x, y, &tracker);
            cx = x;
            cy = y;
            emitted = true;
            previous_verb = verb;
            break;
        }
        case 'Z':
        case 'z': {
            if (!emit(out, RETICLE_PATH_CLOSE, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f))
                return false;
            cx = start_x;
            cy = start_y;
            emitted = true;
            previous_verb = verb;
            break;
        }
        default:
            return false;
        }
    }

    if (!emitted)
        return false;
    if (bounds) {
        if (tracker.any) {
            *bounds = tracker.rect;
        } else {
            *bounds = (flux_rect){0.0f, 0.0f, 0.0f, 0.0f};
        }
    }
    return true;
}

/* ---- Basic shapes ----------------------------------------------------- */

/* Elliptical arc from angle a0 to a1 as ≤90° cubics, emitted already
 * transformed. Shared by ellipse and rounded rect. */
static bool emit_elliptic_arc(svg_cmd_buf *out, svg_transform ctm, float cx, float cy, float rx,
                              float ry, float a0, float a1, bounds_tracker *tracker, float from_x,
                              float from_y) {
    const float half_pi = 3.14159265358979323846f * 0.5f;
    float delta = a1 - a0;
    /* The tolerance is load-bearing: 4*half_pi - 3*half_pi evaluates to
     * 1.5707965f in float, which divides to 1.0000001 and would round an exact
     * quarter arc up to two segments. A circle would then bake five cubics
     * instead of four, and every mask and cursor sprite derived from it would
     * carry the extra seam. */
    const float arc_segments = fabsf(delta) / half_pi;
    uint32_t segments = (uint32_t)ceilf(arc_segments - 1e-4f);
    if (segments == 0)
        segments = 1;
    if (segments > 4)
        segments = 4;
    float step = delta / (float)segments;
    float prev_x = from_x, prev_y = from_y;
    for (uint32_t i = 0; i < segments; ++i) {
        float t1 = a0 + step * (float)i;
        float t2 = t1 + step;
        float alpha = 4.0f / 3.0f * tanf(step * 0.25f);
        float cos1 = cosf(t1), sin1 = sinf(t1);
        float cos2 = cosf(t2), sin2 = sinf(t2);
        float u1 = rx * (cos1 - alpha * sin1);
        float v1 = ry * (sin1 + alpha * cos1);
        float u2 = rx * (cos2 + alpha * sin2);
        float v2 = ry * (sin2 - alpha * cos2);
        float u3 = rx * cos2;
        float v3 = ry * sin2;
        float p1x = 0.0f, p1y = 0.0f, p2x = 0.0f, p2y = 0.0f, p3x = 0.0f, p3y = 0.0f;
        svg__xform_point(ctm, cx + u1, cy + v1, &p1x, &p1y);
        svg__xform_point(ctm, cx + u2, cy + v2, &p2x, &p2y);
        svg__xform_point(ctm, cx + u3, cy + v3, &p3x, &p3y);
        float s0x = 0.0f, s0y = 0.0f;
        svg__xform_point(ctm, prev_x, prev_y, &s0x, &s0y);
        if (!emit(out, RETICLE_PATH_CUBIC, p1x, p1y, p2x, p2y, p3x, p3y))
            return false;
        track_cubic(tracker, s0x, s0y, p1x, p1y, p2x, p2y, p3x, p3y);
        prev_x = cx + u3;
        prev_y = cy + v3;
    }
    return true;
}

bool svg__emit_rect(svg_cmd_buf *out, svg_transform ctm, float x, float y, float w, float h,
                    float rx, float ry, flux_rect *bounds) {
    if (!isfinite(x) || !isfinite(y) || !isfinite(w) || !isfinite(h))
        return false;
    /* SVG: a negative/zero extent disables the element entirely. */
    if (w <= 0.0f || h <= 0.0f)
        return true;
    if (rx < 0.0f)
        rx = 0.0f;
    if (ry < 0.0f)
        ry = 0.0f;
    if (rx <= 0.0f && ry <= 0.0f) {
        float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f, cx2 = 0.0f, cy2 = 0.0f, dx = 0.0f,
              dy = 0.0f;
        svg__xform_point(ctm, x, y, &ax, &ay);
        svg__xform_point(ctm, x + w, y, &bx, &by);
        svg__xform_point(ctm, x + w, y + h, &cx2, &cy2);
        svg__xform_point(ctm, x, y + h, &dx, &dy);
        bounds_tracker tracker = {};
        track(&tracker, ax, ay);
        track(&tracker, bx, by);
        track(&tracker, cx2, cy2);
        track(&tracker, dx, dy);
        if (!emit(out, RETICLE_PATH_MOVE, ax, ay, 0, 0, 0, 0) ||
            !emit(out, RETICLE_PATH_LINE, bx, by, 0, 0, 0, 0) ||
            !emit(out, RETICLE_PATH_LINE, cx2, cy2, 0, 0, 0, 0) ||
            !emit(out, RETICLE_PATH_LINE, dx, dy, 0, 0, 0, 0) ||
            !emit(out, RETICLE_PATH_CLOSE, 0, 0, 0, 0, 0, 0))
            return false;
        if (bounds)
            *bounds = tracker.rect;
        return true;
    }
    /* One radius given means "same for both axes". */
    if (rx <= 0.0f)
        rx = ry;
    if (ry <= 0.0f)
        ry = rx;
    if (rx > w * 0.5f)
        rx = w * 0.5f;
    if (ry > h * 0.5f)
        ry = h * 0.5f;

    const float half_pi = 3.14159265358979323846f * 0.5f;
    bounds_tracker tracker = {};
    float start_x = x + rx, start_y = y;
    float tx = 0.0f, ty = 0.0f;
    svg__xform_point(ctm, start_x, start_y, &tx, &ty);
    track(&tracker, tx, ty);
    if (!emit(out, RETICLE_PATH_MOVE, tx, ty, 0, 0, 0, 0))
        return false;

    /* Top edge, top-right arc, right edge, bottom-right arc, bottom edge,
     * bottom-left arc, left edge, top-left arc, close. */
    svg__xform_point(ctm, x + w - rx, y, &tx, &ty);
    track(&tracker, tx, ty);
    if (!emit(out, RETICLE_PATH_LINE, tx, ty, 0, 0, 0, 0))
        return false;
    if (!emit_elliptic_arc(out, ctm, x + w - rx, y + ry, rx, ry, -half_pi, 0.0f, &tracker,
                           x + w - rx, y))
        return false;
    svg__xform_point(ctm, x + w, y + h - ry, &tx, &ty);
    track(&tracker, tx, ty);
    if (!emit(out, RETICLE_PATH_LINE, tx, ty, 0, 0, 0, 0))
        return false;
    if (!emit_elliptic_arc(out, ctm, x + w - rx, y + h - ry, rx, ry, 0.0f, half_pi, &tracker, x + w,
                           y + h - ry))
        return false;
    svg__xform_point(ctm, x + rx, y + h, &tx, &ty);
    track(&tracker, tx, ty);
    if (!emit(out, RETICLE_PATH_LINE, tx, ty, 0, 0, 0, 0))
        return false;
    if (!emit_elliptic_arc(out, ctm, x + rx, y + h - ry, rx, ry, half_pi, 2.0f * half_pi, &tracker,
                           x + rx, y + h))
        return false;
    svg__xform_point(ctm, x, y + ry, &tx, &ty);
    track(&tracker, tx, ty);
    if (!emit(out, RETICLE_PATH_LINE, tx, ty, 0, 0, 0, 0))
        return false;
    if (!emit_elliptic_arc(out, ctm, x + rx, y + ry, rx, ry, 2.0f * half_pi, 3.0f * half_pi,
                           &tracker, x, y + ry))
        return false;
    svg__xform_point(ctm, x + rx, y, &tx, &ty);
    track(&tracker, tx, ty);
    if (!emit(out, RETICLE_PATH_CLOSE, 0, 0, 0, 0, 0, 0))
        return false;
    if (bounds)
        *bounds = tracker.rect;
    return true;
}

bool svg__emit_ellipse(svg_cmd_buf *out, svg_transform ctm, float cx, float cy, float rx, float ry,
                       flux_rect *bounds) {
    if (!isfinite(cx) || !isfinite(cy) || !isfinite(rx) || !isfinite(ry))
        return false;
    /* SVG: a zero radius disables the element; a negative one is an error that
     * also disables it. */
    if (rx <= 0.0f || ry <= 0.0f)
        return true;
    bounds_tracker tracker = {};
    float start_x = cx + rx, start_y = cy;
    float tx = 0.0f, ty = 0.0f;
    svg__xform_point(ctm, start_x, start_y, &tx, &ty);
    track(&tracker, tx, ty);
    if (!emit(out, RETICLE_PATH_MOVE, tx, ty, 0, 0, 0, 0))
        return false;
    const float half_pi = 3.14159265358979323846f * 0.5f;
    if (!emit_elliptic_arc(out, ctm, cx, cy, rx, ry, 0.0f, half_pi, &tracker, start_x, start_y))
        return false;
    if (!emit_elliptic_arc(out, ctm, cx, cy, rx, ry, half_pi, 2.0f * half_pi, &tracker, cx,
                           cy + ry))
        return false;
    if (!emit_elliptic_arc(out, ctm, cx, cy, rx, ry, 2.0f * half_pi, 3.0f * half_pi, &tracker,
                           cx - rx, cy))
        return false;
    if (!emit_elliptic_arc(out, ctm, cx, cy, rx, ry, 3.0f * half_pi, 4.0f * half_pi, &tracker, cx,
                           cy - ry))
        return false;
    if (!emit(out, RETICLE_PATH_CLOSE, 0, 0, 0, 0, 0, 0))
        return false;
    if (bounds)
        *bounds = tracker.rect;
    return true;
}

bool svg__emit_line(svg_cmd_buf *out, svg_transform ctm, float x1, float y1, float x2, float y2,
                    flux_rect *bounds) {
    if (!isfinite(x1) || !isfinite(y1) || !isfinite(x2) || !isfinite(y2))
        return false;
    bounds_tracker tracker = {};
    float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f;
    svg__xform_point(ctm, x1, y1, &ax, &ay);
    svg__xform_point(ctm, x2, y2, &bx, &by);
    track(&tracker, ax, ay);
    track(&tracker, bx, by);
    if (!emit(out, RETICLE_PATH_MOVE, ax, ay, 0, 0, 0, 0) ||
        !emit(out, RETICLE_PATH_LINE, bx, by, 0, 0, 0, 0))
        return false;
    if (bounds)
        *bounds = tracker.rect;
    return true;
}

bool svg__emit_poly(svg_cmd_buf *out, svg_transform ctm, const float *xy, uint32_t points,
                    bool closed, flux_rect *bounds) {
    if (!xy || points == 0)
        return true;
    bounds_tracker tracker = {};
    for (uint32_t i = 0; i < points; ++i) {
        float tx = 0.0f, ty = 0.0f;
        svg__xform_point(ctm, xy[i * 2], xy[i * 2 + 1], &tx, &ty);
        if (!isfinite(tx) || !isfinite(ty))
            return false;
        track(&tracker, tx, ty);
        uint8_t verb = i == 0 ? RETICLE_PATH_MOVE : RETICLE_PATH_LINE;
        if (!emit(out, verb, tx, ty, 0, 0, 0, 0))
            return false;
    }
    if (closed && !emit(out, RETICLE_PATH_CLOSE, 0, 0, 0, 0, 0, 0))
        return false;
    if (bounds)
        *bounds = tracker.rect;
    return true;
}
