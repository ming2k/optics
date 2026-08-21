/*
 * Color-space model (ADR-0069).
 *
 * A color space is { primaries, transfer function }. All conversions
 * pass through CIE XYZ: linear RGB -> XYZ uses a matrix derived from
 * the primaries' xy chromaticities (the normative definition of every
 * named space below), and differing white points are bridged with
 * Bradford adaptation. Transfer functions are scalar curves applied
 * per component.
 *
 * Working-space convention: scRGB — linear BT.709, extended range,
 * 1.0 = 80 cd/m². PQ values are in units of 10000 cd/m² (ST 2084).
 */
#include <flux/math.h>

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Validation / equality                                             */
/* ------------------------------------------------------------------ */

static bool xy_sane(float x, float y) {
    /* x + y <= 1: points on the spectral locus (P3 red 0.680+0.320)
     * have z == 0 and are legitimate chromaticities. */
    return x > 0.0f && x < 1.0f && y > 0.0f && y < 1.0f && x + y <= 1.0f;
}

bool flux_color_space_is_valid(flux_color_space cs) {
    switch (cs.transfer) {
    case FLUX_TRANSFER_LINEAR:
    case FLUX_TRANSFER_SRGB:
    case FLUX_TRANSFER_PQ:
    case FLUX_TRANSFER_HLG:
        break;
    case FLUX_TRANSFER_GAMMA:
        if (!(cs.gamma > 0.0f))
            return false;
        break;
    default:
        return false;
    }
    switch (cs.primaries) {
    case FLUX_PRIMARIES_BT709:
    case FLUX_PRIMARIES_DISPLAY_P3:
    case FLUX_PRIMARIES_BT2020:
    case FLUX_PRIMARIES_ADOBE_RGB:
        return true;
    case FLUX_PRIMARIES_CUSTOM: {
        if (!(xy_sane(cs.xy.rx, cs.xy.ry) && xy_sane(cs.xy.gx, cs.xy.gy) &&
              xy_sane(cs.xy.bx, cs.xy.by) && xy_sane(cs.xy.wx, cs.xy.wy)))
            return false;
        /* Reject (near-)degenerate primary triangles: collinear or
         * coincident primaries make the RGB->XYZ matrix singular. */
        float ux = cs.xy.gx - cs.xy.rx, uy = cs.xy.gy - cs.xy.ry;
        float vx = cs.xy.bx - cs.xy.rx, vy = cs.xy.by - cs.xy.ry;
        return fabsf(ux * vy - uy * vx) > 1e-6f;
    }
    default:
        return false;
    }
}

bool flux_color_space_equal(flux_color_space a, flux_color_space b) {
    if (a.primaries != b.primaries || a.transfer != b.transfer)
        return false;
    if (a.transfer == FLUX_TRANSFER_GAMMA && a.gamma != b.gamma)
        return false;
    if (a.primaries == FLUX_PRIMARIES_CUSTOM && memcmp(&a.xy, &b.xy, sizeof(a.xy)) != 0)
        return false;
    return true;
}

/* ------------------------------------------------------------------ */
/*  Transfer functions                                                */
/* ------------------------------------------------------------------ */

static float srgb_encode(float l) {
    if (l <= 0.0f)
        return 0.0f;
    if (l >= 1.0f)
        return 1.0f;
    return l <= 0.0031308f ? l * 12.92f : 1.055f * powf(l, 1.0f / 2.4f) - 0.055f;
}

static float srgb_decode(float e) {
    if (e <= 0.0f)
        return 0.0f;
    if (e >= 1.0f)
        return 1.0f;
    return e <= 0.04045f ? e / 12.92f : powf((e + 0.055f) / 1.055f, 2.4f);
}

/* ST 2084. L in units of 10000 cd/m²; N is the encoded code value. */
static float pq_encode(float l) {
    const float m1 = 2610.0f / 16384.0f;
    const float m2 = 2523.0f / 4096.0f * 128.0f;
    const float c1 = 3424.0f / 4096.0f;
    const float c2 = 2413.0f / 4096.0f * 32.0f;
    const float c3 = 2392.0f / 4096.0f * 32.0f;
    if (l <= 0.0f)
        return 0.0f;
    if (l >= 1.0f)
        return 1.0f;
    float lm1 = powf(l, m1);
    return powf((c1 + c2 * lm1) / (1.0f + c3 * lm1), m2);
}

static float pq_decode(float n) {
    const float m1 = 2610.0f / 16384.0f;
    const float m2 = 2523.0f / 4096.0f * 128.0f;
    const float c1 = 3424.0f / 4096.0f;
    const float c2 = 2413.0f / 4096.0f * 32.0f;
    const float c3 = 2392.0f / 4096.0f * 32.0f;
    if (n <= 0.0f)
        return 0.0f;
    if (n >= 1.0f)
        return 1.0f;
    float nm2 = powf(n, 1.0f / m2);
    float num = nm2 - c1;
    if (num < 0.0f)
        num = 0.0f;
    return powf(num / (c2 - c3 * nm2), 1.0f / m1);
}

/* HLG per BT.2100 table 5 (OETF) / table 6 (OOTF without the system
 * gamma — the display-dependent part belongs to the tone mapper, so
 * encode/decode here are an exact inverse pair for scene light 0..1). */
#define HLG_A 0.17883277f
#define HLG_B (1.0f - 4.0f * HLG_A) /* 0.28466892 */
#define HLG_C 0.55991073f

static float hlg_encode(float l) {
    if (l <= 0.0f)
        return 0.0f;
    if (l <= 1.0f / 12.0f)
        return sqrtf(3.0f * l);
    if (l >= 1.0f)
        return 1.0f;
    return HLG_A * logf(12.0f * l - HLG_B) + HLG_C;
}

static float hlg_decode(float e) {
    if (e <= 0.0f)
        return 0.0f;
    if (e <= 0.5f)
        return e * e / 3.0f;
    if (e >= 1.0f)
        return 1.0f;
    return (expf((e - HLG_C) / HLG_A) + HLG_B) / 12.0f;
}

float flux_transfer_encode(flux_transfer_func tf, float gamma, float linear) {
    switch (tf) {
    case FLUX_TRANSFER_LINEAR:
        return linear;
    case FLUX_TRANSFER_SRGB:
        return srgb_encode(linear);
    case FLUX_TRANSFER_GAMMA:
        return linear <= 0.0f ? 0.0f : powf(linear, 1.0f / gamma);
    case FLUX_TRANSFER_PQ:
        return pq_encode(linear);
    case FLUX_TRANSFER_HLG:
        return hlg_encode(linear);
    default:
        return linear;
    }
}

float flux_transfer_decode(flux_transfer_func tf, float gamma, float encoded) {
    switch (tf) {
    case FLUX_TRANSFER_LINEAR:
        return encoded;
    case FLUX_TRANSFER_SRGB:
        return srgb_decode(encoded);
    case FLUX_TRANSFER_GAMMA:
        return encoded <= 0.0f ? 0.0f : powf(encoded, gamma);
    case FLUX_TRANSFER_PQ:
        return pq_decode(encoded);
    case FLUX_TRANSFER_HLG:
        return hlg_decode(encoded);
    default:
        return encoded;
    }
}

/* ------------------------------------------------------------------ */
/*  Primaries -> XYZ                                                  */
/* ------------------------------------------------------------------ */

/* Normative xy chromaticities (white is D65 for every named space). */
static void named_xy(flux_color_primaries p, float xy[8]) {
    switch (p) {
    case FLUX_PRIMARIES_BT709: {
        float v[8] = {0.64f, 0.33f, 0.30f, 0.60f, 0.15f, 0.06f, 0.3127f, 0.3290f};
        memcpy(xy, v, sizeof(v));
    } break;
    case FLUX_PRIMARIES_DISPLAY_P3: {
        float v[8] = {0.680f, 0.320f, 0.265f, 0.690f, 0.150f, 0.060f, 0.3127f, 0.3290f};
        memcpy(xy, v, sizeof(v));
    } break;
    case FLUX_PRIMARIES_BT2020: {
        float v[8] = {0.708f, 0.292f, 0.170f, 0.797f, 0.131f, 0.046f, 0.3127f, 0.3290f};
        memcpy(xy, v, sizeof(v));
    } break;
    case FLUX_PRIMARIES_ADOBE_RGB: {
        float v[8] = {0.64f, 0.33f, 0.21f, 0.71f, 0.15f, 0.06f, 0.3127f, 0.3290f};
        memcpy(xy, v, sizeof(v));
    } break;
    default: { /* unreachable for validated spaces; BT.709 is the safe fill */
        float v[8] = {0.64f, 0.33f, 0.30f, 0.60f, 0.15f, 0.06f, 0.3127f, 0.3290f};
        memcpy(xy, v, sizeof(v));
    } break;
    }
}

/* RGB -> XYZ matrix from xy chromaticities: primaries as unit-Y
 * columns, column-scaled so that R+G+B lands on the white point. */
static flux_mat3 rgb_to_xyz_from_xy(float rx, float ry, float gx, float gy, float bx, float by,
                                    float wx, float wy) {
    flux_vec3 xr = {rx / ry, 1.0f, (1.0f - rx - ry) / ry};
    flux_vec3 xg = {gx / gy, 1.0f, (1.0f - gx - gy) / gy};
    flux_vec3 xb = {bx / by, 1.0f, (1.0f - bx - by) / by};
    flux_vec3 w = {wx / wy, 1.0f, (1.0f - wx - wy) / wy};

    flux_mat3 m = {{xr.x, xr.y, xr.z, xg.x, xg.y, xg.z, xb.x, xb.y, xb.z}};
    flux_vec3 s = flux_mat3_transform_vec3(flux_mat3_invert(m), w);

    return (flux_mat3){{xr.x * s.x, xr.y * s.x, xr.z * s.x, xg.x * s.y, xg.y * s.y, xg.z * s.y,
                        xb.x * s.z, xb.y * s.z, xb.z * s.z}};
}

static flux_mat3 rgb_to_xyz(flux_color_space cs) {
    float xy[8];
    if (cs.primaries == FLUX_PRIMARIES_CUSTOM) {
        float v[8] = {cs.xy.rx, cs.xy.ry, cs.xy.gx, cs.xy.gy,
                      cs.xy.bx, cs.xy.by, cs.xy.wx, cs.xy.wy};
        memcpy(xy, v, sizeof(v));
    } else {
        named_xy(cs.primaries, xy);
    }
    return rgb_to_xyz_from_xy(xy[0], xy[1], xy[2], xy[3], xy[4], xy[5], xy[6], xy[7]);
}

static flux_vec3 white_xyz(flux_color_space cs) {
    float wx, wy;
    if (cs.primaries == FLUX_PRIMARIES_CUSTOM) {
        wx = cs.xy.wx;
        wy = cs.xy.wy;
    } else {
        wx = 0.3127f;
        wy = 0.3290f; /* D65 */
    }
    return (flux_vec3){wx / wy, 1.0f, (1.0f - wx - wy) / wy};
}

/* ------------------------------------------------------------------ */
/*  Bradford chromatic adaptation                                     */
/* ------------------------------------------------------------------ */

static const flux_mat3 BRADFORD = {{
    0.8951f,
    -0.7502f,
    0.0389f, /* column 0 */
    0.2664f,
    1.7135f,
    -0.0685f, /* column 1 */
    -0.1614f,
    0.0367f,
    1.0296f, /* column 2 */
}};

static flux_mat3 adapt_xyz(flux_vec3 src_white, flux_vec3 dst_white) {
    flux_vec3 cs = flux_mat3_transform_vec3(BRADFORD, src_white);
    flux_vec3 cd = flux_mat3_transform_vec3(BRADFORD, dst_white);
    flux_mat3 d = {{
        cd.x / cs.x,
        0,
        0,
        0,
        cd.y / cs.y,
        0,
        0,
        0,
        cd.z / cs.z,
    }};
    return flux_mat3_multiply(flux_mat3_invert(BRADFORD), flux_mat3_multiply(d, BRADFORD));
}

/* Internal hooks for the ICC parser (src/core/icc.c) — declared in
 * src/math/colorspace_internal.h. */
flux_mat3 flux_colorspace_adapt_xyz(flux_vec3 src_white, flux_vec3 dst_white) {
    return adapt_xyz(src_white, dst_white);
}

flux_mat3 flux_colorspace_rgb_to_xyz(flux_color_space cs) {
    return rgb_to_xyz(cs);
}

/* ------------------------------------------------------------------ */
/*  Public transform builder                                          */
/* ------------------------------------------------------------------ */

bool flux_color_space_transform_matrix(flux_color_space from, flux_color_space to, flux_mat3 *out) {
    if (!out)
        return false;
    *out = flux_mat3_identity();
    if (!flux_color_space_is_valid(from) || !flux_color_space_is_valid(to))
        return false;

    flux_mat3 src = rgb_to_xyz(from);
    flux_mat3 dst_inv = flux_mat3_invert(rgb_to_xyz(to));

    flux_vec3 ws = white_xyz(from);
    flux_vec3 wd = white_xyz(to);
    flux_mat3 mid = flux_mat3_identity();
    if (fabsf(ws.x - wd.x) > 1e-6f || fabsf(ws.z - wd.z) > 1e-6f)
        mid = adapt_xyz(ws, wd);

    *out = flux_mat3_multiply(dst_inv, flux_mat3_multiply(mid, src));
    return true;
}
