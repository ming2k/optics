/*
 * ICC profile parser (ADR-0070).
 *
 * The bounded subset flux consumes: ICC v2/v4 display- and
 * scanner-class RGB profiles, either matrix+TRC (rXYZ/gXYZ/bXYZ +
 * rTRC/gTRC/bTRC) or A2B0 LUT (mft1/mft2/mAB). Two products:
 *
 *   - parametric extraction, when the profile maps exactly onto the
 *     {primaries, transfer} model (flux_icc_profile_color_space);
 *   - a baked 65³ 3D LUT (working-space linear RGB) evaluated at parse
 *     time for everything else.
 *
 * All ICC integers are big-endian; fixed point is s15Fixed16 unless
 * noted. Profile connection space is XYZ D50 (v4) or Lab, converted to
 * XYZ D50 and Bradford-adapted to the D65 working space.
 */
#include "../math/colorspace_internal.h"
#include "internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define ICC_LUT_SIZE 65u

struct flux_icc_profile {
    atomic_uint ref_count;
    bool parametric_ok;
    flux_color_space space; /* valid when parametric_ok */
    float *lut;             /* ICC_LUT_SIZE³ × 3 floats, or NULL */
};

/* ------------------------------------------------------------------ */
/*  Big-endian cursor                                                  */
/* ------------------------------------------------------------------ */

typedef struct icc_cursor {
    const uint8_t *base;
    size_t size;
    size_t pos;
    bool ok;
} icc_cursor;

static uint32_t cur_u32(icc_cursor *c) {
    if (c->pos + 4 > c->size) {
        c->ok = false;
        return 0;
    }
    const uint8_t *p = c->base + c->pos;
    c->pos += 4;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t cur_u16(icc_cursor *c) {
    if (c->pos + 2 > c->size) {
        c->ok = false;
        return 0;
    }
    const uint8_t *p = c->base + c->pos;
    c->pos += 2;
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint8_t cur_u8(icc_cursor *c) {
    if (c->pos + 1 > c->size) {
        c->ok = false;
        return 0;
    }
    return c->base[c->pos++];
}

static float cur_s15f16(icc_cursor *c) {
    return (float)(int32_t)cur_u32(c) / 65536.0f;
}

static float cur_u8f8(icc_cursor *c) {
    return (float)cur_u16(c) / 256.0f;
}

static float cur_u16f16(icc_cursor *c) {
    return (float)cur_u32(c) / 65536.0f;
}

/* ------------------------------------------------------------------ */
/*  Tag table                                                          */
/* ------------------------------------------------------------------ */

typedef struct icc_tag_view {
    const uint8_t *ptr;
    size_t size;
} icc_tag_view;

static bool icc_find_tag(const uint8_t *data, size_t size, uint32_t signature, icc_tag_view *out) {
    if (size < 132)
        return false;
    icc_cursor c = {.base = data, .size = size, .pos = 128, .ok = true};
    uint32_t count = cur_u32(&c);
    if (!c.ok || count > 4096)
        return false;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t sig = cur_u32(&c);
        uint32_t offset = cur_u32(&c);
        uint32_t tag_size = cur_u32(&c);
        if (!c.ok)
            return false;
        if (sig == signature) {
            if ((uint64_t)offset + tag_size > size || tag_size < 12)
                return false;
            out->ptr = data + offset;
            out->size = tag_size;
            return true;
        }
    }
    return false;
}

static uint32_t tag_u32(const uint8_t *p, size_t size, size_t at) {
    if (at + 4 > size)
        return 0;
    return ((uint32_t)p[at] << 24) | ((uint32_t)p[at + 1] << 16) | ((uint32_t)p[at + 2] << 8) |
           p[at + 3];
}

/* ------------------------------------------------------------------ */
/*  Tone reproduction curves                                           */
/* ------------------------------------------------------------------ */

typedef struct icc_trc {
    /* Evaluated form: up to 4096-entry table, or analytic. */
    uint32_t count;    /* 0 = analytic (gamma/parametric) */
    float table[4096]; /* normalised 0..1 */
    int kind;          /* 0 table, 1 gamma, 2 parametric(0..4) */
    float gamma;       /* kind 1 */
    int para_type;     /* kind 2 */
    float p[7];        /* g, a, b, c, d, e, f */
} icc_trc;

static float icc_trc_eval_para(const icc_trc *t, float x) {
    const float g = t->p[0], a = t->p[1], b = t->p[2], cc = t->p[3], d = t->p[4];
    switch (t->para_type) {
    case 0:
        return x <= 0.0f ? 0.0f : powf(x, g);
    case 1:
        return x >= -b / a ? powf(a * x + b, g) : 0.0f;
    case 2:
        return x >= -b / a ? powf(a * x + b, g) + cc : cc;
    case 3:
        return x >= d ? powf(a * x + b, g) : cc * x;
    case 4: {
        const float e = t->p[5], f = t->p[6];
        return x >= d ? powf(a * x + b, g) + e : cc * x + f;
    }
    default:
        return x;
    }
}

static float icc_trc_eval(const icc_trc *t, float x) {
    if (x <= 0.0f)
        return 0.0f;
    if (x >= 1.0f)
        x = 1.0f;
    switch (t->kind) {
    case 1:
        return powf(x, t->gamma);
    case 2:
        return icc_trc_eval_para(t, x);
    default: { /* table */
        if (t->count == 0)
            return x; /* empty curv = identity */
        float pos = x * (float)(t->count - 1);
        uint32_t i = (uint32_t)pos;
        if (i + 1 >= t->count)
            return t->table[t->count - 1];
        float f = pos - (float)i;
        return t->table[i] * (1.0f - f) + t->table[i + 1] * f;
    }
    }
}

/* Parse a curv/para tag body. Returns false on malformed or unsupported. */
static bool icc_trc_parse(icc_cursor *c, icc_trc *out) {
    *out = (icc_trc){0};
    uint32_t sig = cur_u32(c);
    cur_u32(c); /* reserved */
    if (sig == 0x63757276u /* 'curv' */) {
        uint32_t n = cur_u32(c);
        if (n == 0) {
            out->kind = 0; /* identity table */
            out->count = 0;
            return c->ok;
        }
        if (n == 1) {
            out->kind = 1;
            out->gamma = cur_u8f8(c);
            return c->ok;
        }
        if (n > 4096)
            return false;
        out->kind = 0;
        out->count = n;
        for (uint32_t i = 0; i < n; ++i)
            out->table[i] = (float)cur_u16(c) / 65535.0f;
        return c->ok;
    }
    if (sig == 0x70617261u /* 'para' */) {
        uint16_t type = cur_u16(c);
        cur_u16(c); /* reserved */
        static const uint8_t param_count[] = {1, 3, 4, 5, 7};
        if (type > 4)
            return false;
        out->kind = 2;
        out->para_type = type;
        for (uint32_t i = 0; i < param_count[type]; ++i)
            out->p[i] = cur_s15f16(c);
        return c->ok;
    }
    return false;
}

static bool icc_trc_parse_tag(icc_tag_view tag, icc_trc *out) {
    icc_cursor c = {.base = tag.ptr, .size = tag.size, .pos = 0, .ok = true};
    bool r = icc_trc_parse(&c, out);
    return r && c.ok;
}

/* The exact sRGB EOTF as an ICC parametric type-4 curve. */
static bool icc_trc_is_srgb(const icc_trc *t) {
    if (t->kind != 2 || t->para_type != 4)
        return false;
    static const float want[7] = {
        2.4f, 1.0f / 1.055f, 0.055f / 1.055f, 1.0f / 12.92f, 0.04045f, 0.0f, 0.0f,
    };
    for (int i = 0; i < 7; ++i) {
        float d = t->p[i] - want[i];
        if (d < 0.0f)
            d = -d;
        if (d > 1e-3f)
            return false;
    }
    return true;
}

static bool icc_trc_same(const icc_trc *a, const icc_trc *b) {
    if (a->kind != b->kind)
        return false;
    for (unsigned i = 0; i < 16; ++i) {
        float x = (float)i / 15.0f;
        float d = icc_trc_eval(a, x) - icc_trc_eval(b, x);
        if (d < 0.0f)
            d = -d;
        if (d > 1e-3f)
            return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/*  Matrix profile extraction                                          */
/* ------------------------------------------------------------------ */

static bool icc_xyz_tag(const uint8_t *data, size_t size, uint32_t sig, flux_vec3 *out) {
    icc_tag_view tag;
    if (!icc_find_tag(data, size, sig, &tag))
        return false;
    if (tag_u32(tag.ptr, tag.size, 0) != 0x58595A20u /* 'XYZ ' */ || tag.size < 20)
        return false;
    icc_cursor c = {.base = tag.ptr, .size = tag.size, .pos = 8, .ok = true};
    out->x = cur_s15f16(&c);
    out->y = cur_s15f16(&c);
    out->z = cur_s15f16(&c);
    return c.ok;
}

static const flux_vec3 ICC_D50 = {0.9642f, 1.0f, 0.8251f};

/* Match adapted-D65 colorants against the named primaries; returns the
 * named tag or FLUX_PRIMARIES_CUSTOM (xy filled). */
static flux_color_primaries icc_match_primaries(flux_mat3 rgb_to_xyz_d65,
                                                float out_xy[8]) {
    /* Columns -> xy chromaticities. */
    float xy[8];
    for (int col = 0; col < 3; ++col) {
        float x = rgb_to_xyz_d65.m[col * 3 + 0];
        float y = rgb_to_xyz_d65.m[col * 3 + 1];
        float z = rgb_to_xyz_d65.m[col * 3 + 2];
        float sum = x + y + z;
        if (sum <= 1e-6f)
            return FLUX_PRIMARIES_CUSTOM;
        xy[col * 2 + 0] = x / sum;
        xy[col * 2 + 1] = y / sum;
    }
    xy[6] = 0.3127f; /* D65 by construction */
    xy[7] = 0.3290f;
    memcpy(out_xy, xy, sizeof(xy));

    static const struct {
        flux_color_primaries tag;
        float xy[6];
    } named[] = {
        {FLUX_PRIMARIES_BT709, {0.64f, 0.33f, 0.30f, 0.60f, 0.15f, 0.06f}},
        {FLUX_PRIMARIES_DISPLAY_P3, {0.680f, 0.320f, 0.265f, 0.690f, 0.150f, 0.060f}},
        {FLUX_PRIMARIES_BT2020, {0.708f, 0.292f, 0.170f, 0.797f, 0.131f, 0.046f}},
        {FLUX_PRIMARIES_ADOBE_RGB, {0.64f, 0.33f, 0.21f, 0.71f, 0.15f, 0.06f}},
    };
    for (unsigned n = 0; n < sizeof(named) / sizeof(named[0]); ++n) {
        bool match = true;
        for (int i = 0; i < 6 && match; ++i) {
            float d = xy[i] - named[n].xy[i];
            if (d < 0.0f)
                d = -d;
            if (d > 2e-3f)
                match = false;
        }
        if (match)
            return named[n].tag;
    }
    return FLUX_PRIMARIES_CUSTOM;
}

/* ------------------------------------------------------------------ */
/*  LUT profile evaluation (A2B0)                                      */
/* ------------------------------------------------------------------ */

typedef struct icc_lut {
    uint32_t grid;      /* CLUT points per dimension */
    uint32_t in_ents;   /* input/output table entries (mft1/2) */
    uint32_t out_ents;
    icc_trc in_curves[3];
    icc_trc out_curves[3];
    bool has_matrix; /* mft2 / mAB with matrix */
    float matrix[9]; /* row-major PCS-side matrix */
    bool has_clut;
    float *clut; /* grid³ × 3, normalised */
    /* mAB stage order: B -> matrix -> M -> CLUT -> A. For mft1/2 the
     * in/out curves and matrix map onto the same skeleton. */
    icc_trc m_curves[3];
    bool has_m_curves;
    icc_trc a_curves[3];
    bool has_a_curves;
} icc_lut;

static void icc_lut_free(icc_lut *l) {
    free(l->clut);
    l->clut = nullptr;
}

/* Tetrahedral interpolation inside one CLUT cell. */
static void clut_eval(const icc_lut *l, float r, float g, float b, float out[3]) {
    float max_i = (float)(l->grid - 1);
    float fr = r * max_i, fg = g * max_i, fb = b * max_i;
    uint32_t ir = (uint32_t)fr, ig = (uint32_t)fg, ib = (uint32_t)fb;
    if (ir >= l->grid - 1)
        ir = l->grid - 2;
    if (ig >= l->grid - 1)
        ig = l->grid - 2;
    if (ib >= l->grid - 1)
        ib = l->grid - 2;
    float dr = fr - (float)ir, dg = fg - (float)ig, db = fb - (float)ib;
    float g2 = (float)l->grid * l->grid;
    size_t base = ((size_t)ib * g2 + (size_t)ig * l->grid + ir) * 3;
    const float *c000 = l->clut + base;
    const float *c100 = l->clut + base + 3;
    const float *c010 = l->clut + base + (size_t)l->grid * 3;
    const float *c110 = c010 + 3;
    const float *c001 = l->clut + base + (size_t)g2 * 3;
    const float *c101 = c001 + 3;
    const float *c011 = c001 + (size_t)l->grid * 3;
    const float *c111 = c011 + 3;

    for (int ch = 0; ch < 3; ++ch) {
        float v;
        if (dr >= dg && dg >= db) {
            v = c000[ch] + dr * (c100[ch] - c000[ch]) + dg * (c110[ch] - c100[ch]) +
                db * (c111[ch] - c110[ch]);
        } else if (dr >= db && db >= dg) {
            v = c000[ch] + dr * (c100[ch] - c000[ch]) + db * (c101[ch] - c100[ch]) +
                dg * (c111[ch] - c101[ch]);
        } else if (db >= dr && dr >= dg) {
            v = c000[ch] + db * (c001[ch] - c000[ch]) + dr * (c101[ch] - c001[ch]) +
                dg * (c111[ch] - c101[ch]);
        } else if (dg >= dr && dr >= db) {
            v = c000[ch] + dg * (c010[ch] - c000[ch]) + dr * (c110[ch] - c010[ch]) +
                db * (c111[ch] - c110[ch]);
        } else if (dg >= db && db >= dr) {
            v = c000[ch] + dg * (c010[ch] - c000[ch]) + db * (c011[ch] - c010[ch]) +
                dr * (c111[ch] - c011[ch]);
        } else { /* db >= dg >= dr */
            v = c000[ch] + db * (c001[ch] - c000[ch]) + dg * (c011[ch] - c001[ch]) +
                dr * (c111[ch] - c011[ch]);
        }
        out[ch] = v;
    }
}

/* Lab (PCS, encoded 0..1 per channel as in the tag) -> XYZ D50. */
static void lab_to_xyz(const float in[3], float out[3]) {
    float L = in[0] * 100.0f;
    float a = in[1] * 255.0f - 128.0f;
    float b = in[2] * 255.0f - 128.0f;
    float fy = (L + 16.0f) / 116.0f;
    float fx = fy + a / 500.0f;
    float fz = fy - b / 200.0f;
    const float e = 216.0f / 24389.0f;
    const float k = 24389.0f / 27.0f;
    float fx3 = fx * fx * fx;
    float fy3 = fy * fy * fy;
    float fz3 = fz * fz * fz;
    out[0] = ICC_D50.x * (fx3 > e ? fx3 : (116.0f * fx - 16.0f) / k);
    out[1] = ICC_D50.y * (fy3 > e ? fy3 : (116.0f * fy - 16.0f) / k);
    out[2] = ICC_D50.z * (fz3 > e ? fz3 : (116.0f * fz - 16.0f) / k);
}

/* Evaluate the full A2B0 pipeline: encoded device RGB -> PCS XYZ D50. */
static void icc_lut_eval_pcs(const icc_lut *l, float r, float g, float b, float out[3]) {
    float v[3] = {icc_trc_eval(&l->in_curves[0], r), icc_trc_eval(&l->in_curves[1], g),
                  icc_trc_eval(&l->in_curves[2], b)};
    if (l->has_matrix) {
        float w[3];
        for (int row = 0; row < 3; ++row)
            w[row] = l->matrix[row * 3 + 0] * v[0] + l->matrix[row * 3 + 1] * v[1] +
                     l->matrix[row * 3 + 2] * v[2];
        memcpy(v, w, sizeof(w));
    }
    if (l->has_m_curves)
        for (int ch = 0; ch < 3; ++ch)
            v[ch] = icc_trc_eval(&l->m_curves[ch], v[ch]);
    if (l->has_clut) {
        float cl[3];
        clut_eval(l, v[0], v[1], v[2], cl);
        memcpy(v, cl, sizeof(cl));
    }
    if (l->has_a_curves)
        for (int ch = 0; ch < 3; ++ch)
            v[ch] = icc_trc_eval(&l->a_curves[ch], v[ch]);
    else
        for (int ch = 0; ch < 3; ++ch)
            v[ch] = icc_trc_eval(&l->out_curves[ch], v[ch]);
    memcpy(out, v, sizeof(float) * 3);
}

/* ------------------------------------------------------------------ */
/*  A2B0 tag parsing                                                   */
/* ------------------------------------------------------------------ */

static bool icc_parse_mft(icc_tag_view tag, bool sixteen, icc_lut *out) {
    icc_cursor c = {.base = tag.ptr, .size = tag.size, .pos = 0, .ok = true};
    cur_u32(&c); /* 'mft1'/'mft2' */
    cur_u32(&c);
    uint32_t in_ch = cur_u8(&c);
    uint32_t out_ch = cur_u8(&c);
    uint32_t grid = cur_u8(&c);
    cur_u8(&c);
    if (!c.ok || in_ch != 3 || out_ch != 3 || grid < 2)
        return false;
    float matrix[9];
    for (int i = 0; i < 9; ++i)
        matrix[i] = cur_s15f16(&c);
    out->has_matrix = sixteen; /* only mft2 applies the matrix */
    memcpy(out->matrix, matrix, sizeof(matrix));
    out->grid = grid;

    /* mft1 tables are fixed at 256 8-bit entries; mft2 carries u16
     * entry counts. */
    uint32_t in_ents = 256;
    uint32_t out_ents = 256;
    if (sixteen) {
        in_ents = cur_u16(&c);
        out_ents = cur_u16(&c);
        if (in_ents < 2 || out_ents < 2 || in_ents > 4096 || out_ents > 4096)
            return false;
    }
    out->in_ents = in_ents;
    out->out_ents = out_ents;

    for (int ch = 0; ch < 3; ++ch) {
        out->in_curves[ch] = (icc_trc){0};
        out->in_curves[ch].count = in_ents;
        for (uint32_t i = 0; i < in_ents; ++i)
            out->in_curves[ch].table[i] =
                sixteen ? (float)cur_u16(&c) / 65535.0f : (float)cur_u8(&c) / 255.0f;
    }
    size_t clut_entries = (size_t)grid * grid * grid * 3;
    out->clut = malloc(clut_entries * sizeof(float));
    if (!out->clut)
        return false;
    out->has_clut = true;
    for (size_t i = 0; i < clut_entries; ++i)
        out->clut[i] = sixteen ? (float)cur_u16(&c) / 65535.0f : (float)cur_u8(&c) / 255.0f;
    for (int ch = 0; ch < 3; ++ch) {
        out->out_curves[ch] = (icc_trc){0};
        out->out_curves[ch].count = out_ents;
        for (uint32_t i = 0; i < out_ents; ++i)
            out->out_curves[ch].table[i] =
                sixteen ? (float)cur_u16(&c) / 65535.0f : (float)cur_u8(&c) / 255.0f;
    }
    return c.ok;
}

/* mAB curve element: a curv/para structure at the given offset. */
static bool icc_parse_mab_curve(const uint8_t *base, size_t size, size_t offset, icc_trc out[3]) {
    for (int ch = 0; ch < 3; ++ch) {
        icc_cursor c = {.base = base, .size = size, .pos = offset, .ok = true};
        if (!icc_trc_parse(&c, &out[ch]) || !c.ok)
            return false;
        /* Curves are 4-byte aligned; identical storage means all three
         * channels share one element only when explicitly repeated — the
         * spec stores three curves back to back. */
        offset = (c.pos + 3u) & ~3u;
    }
    return true;
}

static bool icc_parse_mab(icc_tag_view tag, icc_lut *out) {
    if (tag.size < 32)
        return false;
    /* lutAtoBType header: sig/reserved(8), in_ch(8), out_ch(9), then
     * five u32 element offsets. A zero offset means "element absent". */
    if (tag.ptr[9] != 3)
        return false;
    uint32_t b_off = tag_u32(tag.ptr, tag.size, 12);
    uint32_t matrix_off = tag_u32(tag.ptr, tag.size, 16);
    uint32_t m_off = tag_u32(tag.ptr, tag.size, 20);
    uint32_t clut_off = tag_u32(tag.ptr, tag.size, 24);
    uint32_t a_off = tag_u32(tag.ptr, tag.size, 28);
    if (!b_off || b_off >= tag.size || (matrix_off && matrix_off + 36 > tag.size) ||
        (m_off && m_off >= tag.size) || (clut_off && clut_off + 20 > tag.size) ||
        (a_off && a_off >= tag.size))
        return false;

    /* A2B processing order: B curves -> matrix -> M curves -> CLUT -> A. */
    if (!icc_parse_mab_curve(tag.ptr, tag.size, b_off, out->in_curves))
        return false;
    if (matrix_off) {
        icc_cursor mc = {.base = tag.ptr, .size = tag.size, .pos = matrix_off, .ok = true};
        for (int i = 0; i < 9; ++i)
            out->matrix[i] = cur_s15f16(&mc);
        if (!mc.ok)
            return false;
        out->has_matrix = true;
    }
    if (m_off) {
        if (!icc_parse_mab_curve(tag.ptr, tag.size, m_off, out->m_curves))
            return false;
        out->has_m_curves = true;
    }
    if (clut_off) {
        uint32_t grid = tag.ptr[clut_off];
        uint32_t precision = tag.ptr[clut_off + 16];
        if (grid < 2 || (precision != 1 && precision != 2))
            return false;
        size_t entries = (size_t)grid * grid * grid * 3;
        out->clut = malloc(entries * sizeof(float));
        if (!out->clut)
            return false;
        out->has_clut = true;
        out->grid = grid;
        icc_cursor cc = {.base = tag.ptr, .size = tag.size, .pos = clut_off + 20, .ok = true};
        for (size_t i = 0; i < entries; ++i)
            out->clut[i] =
                precision == 2 ? (float)cur_u16(&cc) / 65535.0f : (float)cur_u8(&cc) / 255.0f;
        if (!cc.ok)
            return false;
    }
    if (a_off) {
        if (!icc_parse_mab_curve(tag.ptr, tag.size, a_off, out->a_curves))
            return false;
        out->has_a_curves = true;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/*  Profile assembly                                                   */
/* ------------------------------------------------------------------ */

/* Working-space transform for PCS values: XYZ D50 -> linear BT.709. */
static flux_mat3 pcs_to_working(void) {
    flux_color_space scrgb = FLUX_COLOR_SPACE_SCRGB;
    flux_mat3 inv709 = flux_mat3_invert(flux_colorspace_rgb_to_xyz(scrgb));
    flux_vec3 d65 = {0.95047f, 1.0f, 1.08883f};
    return flux_mat3_multiply(inv709, flux_colorspace_adapt_xyz(ICC_D50, d65));
}

/* Bake the 65³ LUT: encoded straight RGB -> working-space straight
 * linear RGB. Stored R-fastest, then G, then B. `lut_pipeline` (A2B0)
 * or the matrix+TRC pair supplies the PCS conversion; `lab_pcs`
 * converts encoded Lab PCS to XYZ D50 first. */
static float *icc_bake_lut(flux_mat3 pcs2work, const icc_lut *lut_pipeline, const icc_trc trc[3],
                           flux_mat3 colorants_pcs, bool lab_pcs) {
    size_t n = (size_t)ICC_LUT_SIZE * ICC_LUT_SIZE * ICC_LUT_SIZE;
    float *lut = malloc(n * 3 * sizeof(float));
    if (!lut)
        return nullptr;
    for (uint32_t bi = 0; bi < ICC_LUT_SIZE; ++bi) {
        for (uint32_t gi = 0; gi < ICC_LUT_SIZE; ++gi) {
            for (uint32_t ri = 0; ri < ICC_LUT_SIZE; ++ri) {
                float r = (float)ri / (float)(ICC_LUT_SIZE - 1);
                float g = (float)gi / (float)(ICC_LUT_SIZE - 1);
                float b = (float)bi / (float)(ICC_LUT_SIZE - 1);
                float pcs[3];
                if (lut_pipeline) {
                    icc_lut_eval_pcs(lut_pipeline, r, g, b, pcs);
                } else {
                    flux_vec3 lin = {icc_trc_eval(&trc[0], r), icc_trc_eval(&trc[1], g),
                                     icc_trc_eval(&trc[2], b)};
                    flux_vec3 xyz = flux_mat3_transform_vec3(colorants_pcs, lin);
                    pcs[0] = xyz.x;
                    pcs[1] = xyz.y;
                    pcs[2] = xyz.z;
                }
                if (lab_pcs) {
                    float xyz[3];
                    lab_to_xyz(pcs, xyz);
                    memcpy(pcs, xyz, sizeof(xyz));
                }
                flux_vec3 w =
                    flux_mat3_transform_vec3(pcs2work, (flux_vec3){pcs[0], pcs[1], pcs[2]});
                size_t at = ((size_t)bi * ICC_LUT_SIZE * ICC_LUT_SIZE +
                             (size_t)gi * ICC_LUT_SIZE + ri) * 3;
                lut[at + 0] = w.x;
                lut[at + 1] = w.y;
                lut[at + 2] = w.z;
            }
        }
    }
    return lut;
}

FLUX_API flux_result flux_icc_profile_create(const void *data, size_t size,
                                             flux_icc_profile **out) {
    if (!data || !out || size < 132)
        return FLUX_ERROR_INVALID_ARGUMENT;
    *out = nullptr;

    icc_cursor h = {.base = data, .size = size, .pos = 0, .ok = true};
    uint32_t declared_size = cur_u32(&h);
    h.pos = 12;
    uint32_t dev_class = cur_u32(&h);
    uint32_t color_space = cur_u32(&h);
    uint32_t pcs = cur_u32(&h);
    h.pos = 36;
    uint32_t sig = cur_u32(&h);
    if (!h.ok || sig != 0x61637370u /* 'acsp' */ || declared_size > size) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "not an ICC profile");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    if ((dev_class != 0x6D6E7472u /* 'mntr' */ && dev_class != 0x73636E72u /* 'scnr' */) ||
        color_space != 0x52474220u /* 'RGB ' */) {
        FLUX_FAIL(FLUX_ERROR_UNSUPPORTED, "ICC profile is not an RGB display/scanner profile");
        return FLUX_ERROR_UNSUPPORTED;
    }
    if (pcs != 0x58595A20u /* 'XYZ ' */ && pcs != 0x4C616220u /* 'Lab ' */) {
        FLUX_FAIL(FLUX_ERROR_UNSUPPORTED, "ICC profile has an unknown PCS");
        return FLUX_ERROR_UNSUPPORTED;
    }

    flux_icc_profile *p = calloc(1, sizeof(*p));
    if (!p)
        return FLUX_ERROR_OUT_OF_MEMORY;
    atomic_init(&p->ref_count, 1u);

    bool is_lab_pcs = pcs == 0x4C616220u;
    icc_tag_view a2b0;
    bool have_lut = icc_find_tag(data, size, 0x41324230u /* 'A2B0' */, &a2b0);

    if (have_lut) {
        icc_lut *lut = calloc(1, sizeof(*lut)); /* ~200 KB of TRC tables */
        if (!lut) {
            free(p);
            return FLUX_ERROR_OUT_OF_MEMORY;
        }
        uint32_t lut_sig = tag_u32(a2b0.ptr, a2b0.size, 0);
        bool ok = false;
        if (lut_sig == 0x6D667431u /* 'mft1' */)
            ok = icc_parse_mft(a2b0, false, lut);
        else if (lut_sig == 0x6D667432u /* 'mft2' */)
            ok = icc_parse_mft(a2b0, true, lut);
        else if (lut_sig == 0x6D414220u /* 'mAB ' */)
            ok = icc_parse_mab(a2b0, lut);
        if (!ok) {
            icc_lut_free(lut);
            free(lut);
            free(p);
            FLUX_FAIL(FLUX_ERROR_UNSUPPORTED, "ICC A2B0 tag is malformed or unsupported");
            return FLUX_ERROR_UNSUPPORTED;
        }
        flux_mat3 conv = pcs_to_working();
        p->lut = icc_bake_lut(conv, lut, nullptr, flux_mat3_identity(), is_lab_pcs);
        icc_lut_free(lut);
        free(lut);
        if (!p->lut) {
            free(p);
            return FLUX_ERROR_OUT_OF_MEMORY;
        }
        p->parametric_ok = false;
        *out = p;
        return FLUX_OK;
    }

    /* Matrix + TRC path. */
    flux_vec3 r_xyz, g_xyz, b_xyz;
    icc_tag_view trc_tags[3];
    static const uint32_t xyz_sigs[3] = {0x7258595Au, 0x6758595Au, 0x6258595Au}; /* rXYZ gXYZ bXYZ */
    static const uint32_t trc_sigs[3] = {0x72545243u, 0x67545243u, 0x62545243u}; /* rTRC gTRC bTRC */
    flux_vec3 *xyz_out[3] = {&r_xyz, &g_xyz, &b_xyz};
    icc_trc trcs[3];
    bool matrix_ok = true;
    for (int i = 0; i < 3; ++i) {
        matrix_ok &= icc_xyz_tag(data, size, xyz_sigs[i], xyz_out[i]);
        matrix_ok &= icc_find_tag(data, size, trc_sigs[i], &trc_tags[i]);
        if (matrix_ok)
            matrix_ok &= icc_trc_parse_tag(trc_tags[i], &trcs[i]);
    }
    if (!matrix_ok) {
        free(p);
        FLUX_FAIL(FLUX_ERROR_UNSUPPORTED, "ICC profile has neither A2B0 nor matrix+TRC tags");
        return FLUX_ERROR_UNSUPPORTED;
    }
    if (is_lab_pcs) {
        free(p);
        FLUX_FAIL(FLUX_ERROR_UNSUPPORTED, "ICC matrix profile with Lab PCS");
        return FLUX_ERROR_UNSUPPORTED;
    }

    /* Colorant columns are D50-relative; adapt to the D65 working white. */
    flux_mat3 colorants = {{
        r_xyz.x, r_xyz.y, r_xyz.z, g_xyz.x, g_xyz.y, g_xyz.z, b_xyz.x, b_xyz.y, b_xyz.z,
    }};
    flux_vec3 d65 = {0.95047f, 1.0f, 1.08883f};
    flux_mat3 adapt = flux_colorspace_adapt_xyz(ICC_D50, d65);
    flux_mat3 colorants_d65 = flux_mat3_multiply(adapt, colorants);

    /* Parametric extraction: identical TRCs in the flux transfer set. */
    if (icc_trc_same(&trcs[0], &trcs[1]) && icc_trc_same(&trcs[1], &trcs[2])) {
        const icc_trc *t = &trcs[0];
        flux_transfer_func tf = FLUX_TRANSFER_LINEAR;
        float gamma = 0.0f;
        bool have_tf = false;
        if (t->kind == 1 || (t->kind == 2 && t->para_type == 0)) {
            tf = FLUX_TRANSFER_GAMMA;
            gamma = t->kind == 1 ? t->gamma : t->p[0];
            have_tf = gamma > 0.0f;
        } else if (icc_trc_is_srgb(t)) {
            tf = FLUX_TRANSFER_SRGB;
            have_tf = true;
        } else if (t->kind == 0 && t->count == 0) {
            tf = FLUX_TRANSFER_LINEAR;
            have_tf = true;
        }
        if (have_tf) {
            float xy[8];
            flux_color_primaries prim = icc_match_primaries(colorants_d65, xy);
            p->space = (flux_color_space){0};
            p->space.primaries = prim;
            p->space.transfer = tf;
            p->space.gamma = gamma;
            if (prim == FLUX_PRIMARIES_CUSTOM) {
                p->space.xy.rx = xy[0];
                p->space.xy.ry = xy[1];
                p->space.xy.gx = xy[2];
                p->space.xy.gy = xy[3];
                p->space.xy.bx = xy[4];
                p->space.xy.by = xy[5];
                p->space.xy.wx = xy[6];
                p->space.xy.wy = xy[7];
            }
            if (flux_color_space_is_valid(p->space)) {
                p->parametric_ok = true;
                *out = p;
                return FLUX_OK;
            }
        }
    }

    /* Not exactly representable: bake through the matrix path. */
    flux_mat3 conv = pcs_to_working();
    p->lut = icc_bake_lut(conv, nullptr, trcs, colorants, false);
    if (!p->lut) {
        free(p);
        return FLUX_ERROR_OUT_OF_MEMORY;
    }
    p->parametric_ok = false;
    *out = p;
    return FLUX_OK;
}

FLUX_API flux_icc_profile *flux_icc_profile_retain(flux_icc_profile *p) {
    if (p)
        atomic_fetch_add_explicit(&p->ref_count, 1u, memory_order_relaxed);
    return p;
}

FLUX_API void flux_icc_profile_release(flux_icc_profile *p) {
    if (!p)
        return;
    if (atomic_fetch_sub_explicit(&p->ref_count, 1u, memory_order_acq_rel) != 1u)
        return;
    free(p->lut);
    free(p);
}

FLUX_API bool flux_icc_profile_color_space(const flux_icc_profile *p, flux_color_space *out) {
    if (!p || !out || !p->parametric_ok)
        return false;
    *out = p->space;
    return true;
}

/* ------------------------------------------------------------------ */
/*  Internal consumers (image creation)                                */
/* ------------------------------------------------------------------ */

const float *flux_icc_profile_lut(const flux_icc_profile *p, uint32_t *out_size) {
    if (!p || !p->lut)
        return nullptr;
    if (out_size)
        *out_size = ICC_LUT_SIZE;
    return p->lut;
}
