/*
 * flux color-management shared GLSL (ADR-0069).
 *
 * Ports of the scalar transfer functions in src/math/colorspace.c —
 * keep the two in sync. Values flowing through the canvas/scene
 * pipelines are premultiplied linear light in the working space
 * (scRGB: BT.709 primaries, 1.0 = 80 cd/m²). Encoded content is
 * premultiplied in its encoded space; conversion un-premultiplies,
 * applies the per-channel curve, and re-premultiplies.
 *
 * Transfer ids match flux_transfer_func in <flux/core.h>.
 */

#ifndef FLUX_COLOR_GLSL
#define FLUX_COLOR_GLSL

#define FLUX_TF_LINEAR 0
#define FLUX_TF_SRGB   1
#define FLUX_TF_GAMMA  2
#define FLUX_TF_PQ     3
#define FLUX_TF_HLG    4

/* flux_canvas_push.kind high bits (src/canvas/internal.h). */
#define FLUX_CANVAS_PUSH_DECODE_SRGB 0x100u
/* color_params holds a live buffer reference (ADR-0070). */
#define FLUX_CANVAS_PUSH_HAS_COLOR_PARAMS 0x200u

float flux_srgb_encode(float l) {
    if (l <= 0.0)
        return 0.0;
    if (l >= 1.0)
        return 1.0;
    return l <= 0.0031308 ? l * 12.92 : 1.055 * pow(l, 1.0 / 2.4) - 0.055;
}

float flux_srgb_decode(float e) {
    if (e <= 0.0)
        return 0.0;
    if (e >= 1.0)
        return 1.0;
    return e <= 0.04045 ? e / 12.92 : pow((e + 0.055) / 1.055, 2.4);
}

/* ST 2084. Linear is in units of 10000 cd/m². */
float flux_pq_encode(float l) {
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 4096.0 * 128.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 4096.0 * 32.0;
    const float c3 = 2392.0 / 4096.0 * 32.0;
    if (l <= 0.0)
        return 0.0;
    if (l >= 1.0)
        return 1.0;
    float lm1 = pow(l, m1);
    return pow((c1 + c2 * lm1) / (1.0 + c3 * lm1), m2);
}

float flux_pq_decode(float n) {
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 4096.0 * 128.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 4096.0 * 32.0;
    const float c3 = 2392.0 / 4096.0 * 32.0;
    if (n <= 0.0)
        return 0.0;
    if (n >= 1.0)
        return 1.0;
    float nm2 = pow(n, 1.0 / m2);
    return pow(max(nm2 - c1, 0.0) / (c2 - c3 * nm2), 1.0 / m1);
}

/* HLG per BT.2100 (scene-light inverse pair; the display system gamma
 * belongs to the tone mapper). */
float flux_hlg_encode(float l) {
    const float a = 0.17883277;
    const float b = 1.0 - 4.0 * a;
    const float c = 0.55991073;
    if (l <= 0.0)
        return 0.0;
    if (l <= 1.0 / 12.0)
        return sqrt(3.0 * l);
    if (l >= 1.0)
        return 1.0;
    return a * log(12.0 * l - b) + c;
}

float flux_hlg_decode(float e) {
    const float a = 0.17883277;
    const float b = 1.0 - 4.0 * a;
    const float c = 0.55991073;
    if (e <= 0.0)
        return 0.0;
    if (e <= 0.5)
        return e * e / 3.0;
    if (e >= 1.0)
        return 1.0;
    return (exp((e - c) / a) + b) / 12.0;
}

float flux_tf_encode(int tf, float gamma, float l) {
    switch (tf) {
    case FLUX_TF_SRGB:
        return flux_srgb_encode(l);
    case FLUX_TF_GAMMA:
        return l <= 0.0 ? 0.0 : pow(l, 1.0 / gamma);
    case FLUX_TF_PQ:
        return flux_pq_encode(l);
    case FLUX_TF_HLG:
        return flux_hlg_encode(l);
    default:
        return l;
    }
}

float flux_tf_decode(int tf, float gamma, float e) {
    switch (tf) {
    case FLUX_TF_SRGB:
        return flux_srgb_decode(e);
    case FLUX_TF_GAMMA:
        return e <= 0.0 ? 0.0 : pow(e, gamma);
    case FLUX_TF_PQ:
        return flux_pq_decode(e);
    case FLUX_TF_HLG:
        return flux_hlg_decode(e);
    default:
        return e;
    }
}

vec3 flux_tf_encode3(int tf, float gamma, vec3 l) {
    return vec3(flux_tf_encode(tf, gamma, l.r), flux_tf_encode(tf, gamma, l.g),
                flux_tf_encode(tf, gamma, l.b));
}

vec3 flux_tf_decode3(int tf, float gamma, vec3 e) {
    return vec3(flux_tf_decode(tf, gamma, e.r), flux_tf_decode(tf, gamma, e.g),
                flux_tf_decode(tf, gamma, e.b));
}

/* Nominal reference white for ITU-R BT.2408 SDR graphics white in HDR (203 cd/m²). */
#define FLUX_SDR_WHITE_NITS_DEFAULT 203.0

/* Decode straight encoded colour into the linear working space (where 1.0 = SDR white). */
vec3 flux_decode_to_working(int tf, float gamma, vec3 e) {
    vec3 lin = flux_tf_decode3(tf, gamma, e);
    if (tf == FLUX_TF_PQ)
        lin *= (10000.0 / FLUX_SDR_WHITE_NITS_DEFAULT);
    else if (tf == FLUX_TF_HLG)
        lin *= (1000.0 / FLUX_SDR_WHITE_NITS_DEFAULT);
    return lin;
}

/* Premultiplied sRGB -> premultiplied linear (working space). */
vec4 flux_decode_premul_srgb(vec4 c) {
    float a = c.a;
    vec3 straight = a > 0.0 ? c.rgb / a : vec3(0.0);
    return vec4(flux_tf_decode3(FLUX_TF_SRGB, 0.0, straight) * a, a);
}

/* Premultiplied linear (working space) -> premultiplied encoded. */
vec4 flux_encode_premul(int tf, float gamma, vec4 c) {
    float a = c.a;
    vec3 straight = a > 0.0 ? c.rgb / a : vec3(0.0);
    return vec4(flux_tf_encode3(tf, gamma, straight) * a, a);
}

/* HDR->SDR highlight rolloff (ADR-0069): working-space values above 1.0
 * are compressed through a shoulder, scaling by the max component so
 * hue survives. Identity in the SDR range. */
vec3 flux_tonemap_shoulder(vec3 c) {
    float m = max(c.r, max(c.g, c.b));
    if (m <= 1.0)
        return c;
    float compressed = 1.0 + (m - 1.0) / (1.0 + (m - 1.0));
    return c * (compressed / m);
}

/* ITU-R BT.2390 EDR highlight rolloff: compresses values above knee up to
 * headroom with a smooth Hermite spline, preserving hue by scaling max component. */
vec3 flux_tonemap_bt2390_shoulder(vec3 c, float knee, float headroom) {
    if (headroom <= knee)
        return clamp(c, 0.0, headroom);
    float m = max(c.r, max(c.g, c.b));
    if (m <= knee)
        return c;
    float t = clamp((m - knee) / (headroom - knee), 0.0, 1.0);
    float h = 3.0 * t * t - 2.0 * t * t * t;
    float compressed = mix(knee, headroom, h);
    return c * (compressed / m);
}

/* Triangular-PDF dither, ±1 LSB of `levels`, keyed on the fragment
 * coordinate. Kills banding when quantising linear gradients to 8 bit. */
float flux_dither_tpdf(vec2 frag_coord, float levels) {
    vec2 p = fract(frag_coord * vec2(0.7548776662, 0.5698402910));
    float r = fract(p.x + p.y + p.x * p.y * 52.0);
    float r2 = fract(r * 1.3247179572);
    return (r + r2 - 1.0) / levels;
}

#endif /* FLUX_COLOR_GLSL */
