/*
 * Coverage acceptance harness (ADR-0119, S0). See coverage_harness.h for the
 * design contract and for what this reference is scoped to prove.
 *
 * Contours: flux_curve_cover is a FLAT segment run with no contour delimiter, so
 * this harness splits it the only way the representation allows — a break in the
 * chain, where segment i's end differs from segment i+1's start. Each contour is
 * flattened and clipped independently, and the per-pixel areas are summed. That
 * sum is what makes holes work: a reversed inner contour contributes negative
 * area, because clipping a polygon preserves its orientation.
 *
 * The area reference works per row then per column: clip the flattened contour
 * to the pixel row's horizontal band once, then to each pixel's vertical band,
 * and take the signed area of what survives.
 */

#include "coverage_harness.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Fixed capacity: a reference must fail loudly rather than silently truncate. */
#define COV_MAX_PTS 16384u

/* The harness reports through its return value only. libflux keeps its error
 * sink hidden, and a test utility has no business reaching into it: every
 * caller here is a test that asserts on the returned code. */

bool cov_plane_init(cov_plane *p, uint32_t w, uint32_t h) {
    if (!p || w == 0 || h == 0)
        return false;
    p->px = calloc((size_t)w * h, sizeof(float));
    if (!p->px)
        return false;
    p->w = w;
    p->h = h;
    return true;
}

void cov_plane_free(cov_plane *p) {
    if (!p)
        return;
    free(p->px);
    p->px = nullptr;
    p->w = p->h = 0;
}

/* ------------------------------------------------------------------ */
/*  Contour splitting                                                  */
/* ------------------------------------------------------------------ */

/* One past the last segment of the contour beginning at `start`. A contour ends
 * where the chain breaks: segment i's end is not segment i+1's start. */
static uint32_t contour_end(const flux_curve_cover *c, uint32_t start) {
    uint32_t i = start;
    while (i + 1 < c->count) {
        const flux_curve_seg *a = &c->segs[i];
        const flux_curve_seg *b = &c->segs[i + 1];
        if (a->p2.x != b->p0.x || a->p2.y != b->p0.y)
            break;
        ++i;
    }
    return i + 1;
}

/* ------------------------------------------------------------------ */
/*  Flattening (harness-local, so it cannot drift with a backend)      */
/* ------------------------------------------------------------------ */

static bool flatten_seg(flux_curve_seg s, flux_point *out, uint32_t cap, uint32_t *n) {
    uint32_t k =
        (s.op == FLUX_CURVE_LINE) ? 1u : flux_curve_wang_quad(s.p0, s.p1, s.p2, COV_FLATTEN_TOL);
    if (k < 1)
        k = 1;
    if (k > 512)
        k = 512;
    for (uint32_t i = 1; i <= k; ++i) {
        if (*n >= cap)
            return false;
        out[(*n)++] = flux_curve_quad_eval(s, (float)i / (float)k);
    }
    return true;
}

/* Flatten one contour [start, end) into `out`, beginning at its first point. */
static bool flatten_contour(const flux_curve_cover *c, uint32_t start, uint32_t end,
                            flux_point *out, uint32_t cap, uint32_t *n) {
    *n = 0;
    if (*n >= cap)
        return false;
    out[(*n)++] = c->segs[start].p0;
    for (uint32_t i = start; i < end; ++i)
        if (!flatten_seg(c->segs[i], out, cap, n))
            return false;
    /* A closed contour repeats its first point. Drop the repeat: the implicit
     * closing edge reconstructs it, and leaving it in creates a zero-length edge
     * that makes adjacency tests (and area accumulation) see a phantom vertex. */
    if (*n > 1 && out[0].x == out[*n - 1].x && out[0].y == out[*n - 1].y)
        (*n)--;
    return true;
}

/* Proper crossings between non-adjacent edges. The area reference clips and
 * sums signed areas, which is only meaningful for a SIMPLE contour: clipping a
 * self-intersecting polygon against a half-plane produces fragments whose signed
 * areas cancel, silently yielding a small or zero plane. A yardstick that
 * returns 0 must never be trusted — a future gate comparing against it would
 * pass anything — so this is detected and reported instead. */
static bool contour_self_intersects(const flux_point *p, uint32_t n) {
    if (n < 4)
        return false;
    for (uint32_t i = 0; i < n; ++i) {
        flux_point a0 = p[i], a1 = p[(i + 1) % n];
        for (uint32_t j = i + 2; j < n; ++j) {
            if (i == 0 && j == n - 1)
                continue; /* adjacent through the closing edge */
            flux_point b0 = p[j], b1 = p[(j + 1) % n];
            double d1 =
                (double)(a1.x - a0.x) * (b0.y - a0.y) - (double)(a1.y - a0.y) * (b0.x - a0.x);
            double d2 =
                (double)(a1.x - a0.x) * (b1.y - a0.y) - (double)(a1.y - a0.y) * (b1.x - a0.x);
            double d3 =
                (double)(b1.x - b0.x) * (a0.y - b0.y) - (double)(b1.y - b0.y) * (a0.x - b0.x);
            double d4 =
                (double)(b1.x - b0.x) * (a1.y - b0.y) - (double)(b1.y - b0.y) * (a1.x - b0.x);
            /* Strict straddle: d == 0 means an endpoint lies exactly on the
             * other edge, which for adjacent or touching edges is not a
             * crossing. Requiring opposite SIGNS keeps a shared vertex from
             * registering as self-intersection. */
            bool s1 = (d1 > 0.0 && d2 < 0.0) || (d1 < 0.0 && d2 > 0.0);
            bool s2 = (d3 > 0.0 && d4 < 0.0) || (d3 < 0.0 && d4 > 0.0);
            if (s1 && s2)
                return true;
        }
    }
    return false;
}

static double signed_area_of(const flux_point *p, uint32_t n) {
    if (n < 3)
        return 0.0;
    double a = 0.0;
    for (uint32_t i = 0, j = n - 1; i < n; j = i++)
        a += (double)p[j].x * (double)p[i].y - (double)p[i].x * (double)p[j].y;
    return 0.5 * a;
}

double cov_harness_signed_area(flux_curve_cover cover) {
    if (!cover.segs || cover.count == 0)
        return 0.0;
    flux_point *poly = malloc(COV_MAX_PTS * sizeof(flux_point));
    if (!poly)
        return 0.0;
    double area = 0.0;
    for (uint32_t s = 0; s < cover.count;) {
        uint32_t e = contour_end(&cover, s);
        uint32_t n = 0;
        if (!flatten_contour(&cover, s, e, poly, COV_MAX_PTS, &n)) {
            free(poly);
            return 0.0;
        }
        area += signed_area_of(poly, n);
        s = e;
    }
    free(poly);
    return area;
}

/* ------------------------------------------------------------------ */
/*  Sutherland-Hodgman against one axis-aligned half-plane             */
/* ------------------------------------------------------------------ */
static uint32_t clip_half(const flux_point *in, uint32_t n, int axis, float v, bool keep_ge,
                          flux_point *out, uint32_t cap) {
    if (n == 0)
        return 0;
    uint32_t m = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const flux_point *a = &in[i];
        const flux_point *b = &in[(i + 1) % n];
        float av = axis == 0 ? a->x : a->y;
        float bv = axis == 0 ? b->x : b->y;
        bool ain = keep_ge ? (av >= v) : (av <= v);
        bool bin = keep_ge ? (bv >= v) : (bv <= v);
        if (ain) {
            if (m >= cap)
                return 0;
            out[m++] = *a;
        }
        if (ain != bin) {
            float t = (v - av) / (bv - av);
            flux_point c = {a->x + t * (b->x - a->x), a->y + t * (b->y - a->y)};
            if (m >= cap)
                return 0;
            out[m++] = c;
        }
    }
    return m;
}

/* ------------------------------------------------------------------ */
/*  Exact-area reference                                               */
/* ------------------------------------------------------------------ */

flux_result cov_harness_area(flux_curve_cover cover, float ox, float oy, uint32_t w, uint32_t h,
                             cov_plane *out) {
    if (!out || !out->px || out->w != w || out->h != h || w == 0 || h == 0)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (!cover.segs || cover.count == 0) {
        memset(out->px, 0, (size_t)w * h * sizeof(float));
        return FLUX_OK;
    }

    /* Distinct buffers everywhere: clip_half must never read what it writes, and
     * the source contour must survive every row. */
    flux_point *poly = malloc(COV_MAX_PTS * sizeof(flux_point));
    flux_point *band = malloc(COV_MAX_PTS * sizeof(flux_point));
    flux_point *cell = malloc(COV_MAX_PTS * sizeof(flux_point));
    flux_point *fin = malloc(COV_MAX_PTS * sizeof(flux_point));
    if (!poly || !band || !cell || !fin) {
        free(poly);
        free(band);
        free(cell);
        free(fin);
        return FLUX_ERROR_OUT_OF_MEMORY;
    }
    memset(out->px, 0, (size_t)w * h * sizeof(float));

    flux_result rc = FLUX_OK;
    for (uint32_t s = 0; s < cover.count && rc == FLUX_OK;) {
        uint32_t e = contour_end(&cover, s);
        uint32_t n = 0;
        if (!flatten_contour(&cover, s, e, poly, COV_MAX_PTS, &n)) {
            rc = FLUX_ERROR_OUT_OF_RANGE;
            break;
        }
        if (contour_self_intersects(poly, n)) {
            rc = FLUX_ERROR_UNSUPPORTED;
            break;
        }
        for (uint32_t j = 0; j < h && rc == FLUX_OK; ++j) {
            float y0 = oy + (float)j, y1 = y0 + 1.0f;
            uint32_t nb = clip_half(poly, n, 1, y0, true, band, COV_MAX_PTS);
            if (nb == 0)
                continue; /* this row misses the contour */
            uint32_t nb2 = clip_half(band, nb, 1, y1, false, cell, COV_MAX_PTS);
            if (nb2 == 0)
                continue;
            for (uint32_t i = 0; i < w; ++i) {
                float x0 = ox + (float)i, x1 = x0 + 1.0f;
                uint32_t c1 = clip_half(cell, nb2, 0, x0, true, band, COV_MAX_PTS);
                if (c1 == 0)
                    continue;
                uint32_t c2 = clip_half(band, c1, 0, x1, false, fin, COV_MAX_PTS);
                if (c2 == 0)
                    continue;
                double a = signed_area_of(fin, c2);
                if (a != 0.0)
                    out->px[(size_t)j * w + i] += (float)a;
            }
        }
        s = e;
    }

    for (size_t k = 0; k < (size_t)w * h; ++k) {
        if (out->px[k] < 0.0f)
            out->px[k] = 0.0f;
        if (out->px[k] > 1.0f)
            out->px[k] = 1.0f;
    }

    free(poly);
    free(band);
    free(cell);
    free(fin);
    return rc;
}

/* ------------------------------------------------------------------ */
/*  Supersampled reference                                             */
/* ------------------------------------------------------------------ */

flux_result cov_harness_supersample(flux_curve_cover cover, flux_curve_fill rule, float ox,
                                    float oy, uint32_t w, uint32_t h, uint32_t n, cov_plane *out) {
    if (!out || !out->px || out->w != w || out->h != h || w == 0 || h == 0)
        return FLUX_ERROR_INVALID_ARGUMENT;
    for (uint32_t j = 0; j < h; ++j)
        for (uint32_t i = 0; i < w; ++i)
            out->px[(size_t)j * w + i] =
                flux_curve_cover_coverage(cover, rule, ox + (float)i, oy + (float)j, n);
    return FLUX_OK;
}

/* ------------------------------------------------------------------ */
/*  Reporting                                                          */
/* ------------------------------------------------------------------ */

int cov_harness_levels(const cov_plane *plane) {
    if (!plane || !plane->px)
        return 0;
    unsigned char seen[256] = {0};
    for (size_t k = 0; k < (size_t)plane->w * plane->h; ++k) {
        float c = plane->px[k];
        if (c < 0.0f)
            c = 0.0f;
        if (c > 1.0f)
            c = 1.0f;
        seen[(unsigned char)(c * 255.0f + 0.5f)] = 1;
    }
    int levels = 0;
    for (int i = 0; i < 256; ++i)
        levels += seen[i];
    return levels;
}

void cov_harness_compare(const cov_plane *got, const cov_plane *ref, float threshold,
                         cov_report *out) {
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (!got || !ref || !got->px || !ref->px || got->w != ref->w || got->h != ref->h)
        return;
    double sum = 0.0;
    float worst = 0.0f;
    uint32_t over = 0;
    size_t total = (size_t)got->w * got->h;
    for (size_t k = 0; k < total; ++k) {
        float d = fabsf(got->px[k] - ref->px[k]);
        sum += (double)d;
        if (d > worst)
            worst = d;
        if (d > threshold)
            over++;
    }
    out->mean_abs = total ? (float)(sum / (double)total) : 0.0f;
    out->max_abs = worst;
    out->over = over;
    out->total = (uint32_t)total;
    out->levels = cov_harness_levels(got);
    out->clamped = false;

    /* Per-row totals: the localisation signal. */
    double worst_row_mag = 0.0;
    out->worst_row = 0;
    out->worst_row_delta = 0.0;
    for (uint32_t j = 0; j < got->h; ++j) {
        double sg = 0.0, sr = 0.0;
        for (uint32_t i = 0; i < got->w; ++i) {
            sg += (double)got->px[(size_t)j * got->w + i];
            sr += (double)ref->px[(size_t)j * ref->w + i];
        }
        double d = sg - sr;
        double mag = d < 0.0 ? -d : d;
        if (mag > worst_row_mag) {
            worst_row_mag = mag;
            out->worst_row = j;
            out->worst_row_delta = d;
        }
    }
}
