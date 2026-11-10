/*
 * canvas_color.slang — flux color-management shared Slang module (ADR-0069).
 *
 * Ports of the scalar transfer functions in src/math/colorspace.c.
 * Values flowing through the canvas/scene pipelines are premultiplied
 * linear light in the working space (scRGB: BT.709 primaries, 1.0 = 80 cd/m²).
 */

#ifndef FLUX_COLOR_SLANG
#define FLUX_COLOR_SLANG

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
    if (l <= 0.0f)
        return 0.0f;
    if (l >= 1.0f)
        return 1.0f;
    return l <= 0.0031308f ? l * 12.92f : 1.055f * pow(l, 1.0f / 2.4f) - 0.055f;
}

float flux_srgb_decode(float e) {
    if (e <= 0.0f)
        return 0.0f;
    if (e >= 1.0f)
        return 1.0f;
    return e <= 0.04045f ? e / 12.92f : pow((e + 0.055f) / 1.055f, 2.4f);
}

/* ST 2084. Linear is in units of 10000 cd/m². */
float flux_pq_encode(float l) {
    const float m1 = 2610.0f / 16384.0f;
    const float m2 = 2523.0f / 4096.0f * 128.0f;
    const float c1 = 3424.0f / 4096.0f;
    const float c2 = 2413.0f / 4096.0f * 32.0f;
    const float c3 = 2392.0f / 4096.0f * 32.0f;
    if (l <= 0.0f)
        return 0.0f;
    if (l >= 1.0f)
        return 1.0f;
    float lm1 = pow(l, m1);
    return pow((c1 + c2 * lm1) / (1.0f + c3 * lm1), m2);
}

float flux_pq_decode(float n) {
    const float m1 = 2610.0f / 16384.0f;
    const float m2 = 2523.0f / 4096.0f * 128.0f;
    const float c1 = 3424.0f / 4096.0f;
    const float c2 = 2413.0f / 4096.0f * 32.0f;
    const float c3 = 2392.0f / 4096.0f * 32.0f;
    if (n <= 0.0f)
        return 0.0f;
    if (n >= 1.0f)
        return 1.0f;
    float nm2 = pow(n, 1.0f / m2);
    return pow(max(nm2 - c1, 0.0f) / (c2 - c3 * nm2), 1.0f / m1);
}

/* HLG per BT.2100 (scene-light inverse pair; the display system gamma
 * belongs to the tone mapper). */
float flux_hlg_encode(float l) {
    const float a = 0.17883277f;
    const float b = 1.0f - 4.0f * a;
    const float c = 0.55991073f;
    if (l <= 0.0f)
        return 0.0f;
    if (l <= 1.0f / 12.0f)
        return sqrt(3.0f * l);
    if (l >= 1.0f)
        return 1.0f;
    return a * log(12.0f * l - b) + c;
}

float flux_hlg_decode(float e) {
    const float a = 0.17883277f;
    const float b = 1.0f - 4.0f * a;
    const float c = 0.55991073f;
    if (e <= 0.0f)
        return 0.0f;
    if (e <= 0.5f)
        return e * e / 3.0f;
    if (e >= 1.0f)
        return 1.0f;
    return (exp((e - c) / a) + b) / 12.0f;
}

float flux_tf_encode(int tf, float gamma, float l) {
    switch (tf) {
    case FLUX_TF_SRGB:
        return flux_srgb_encode(l);
    case FLUX_TF_GAMMA:
        return l <= 0.0f ? 0.0f : pow(l, 1.0f / gamma);
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
        return e <= 0.0f ? 0.0f : pow(e, gamma);
    case FLUX_TF_PQ:
        return flux_pq_decode(e);
    case FLUX_TF_HLG:
        return flux_hlg_decode(e);
    default:
        return e;
    }
}

float3 flux_tf_encode3(int tf, float gamma, float3 l) {
    return float3(flux_tf_encode(tf, gamma, l.r), flux_tf_encode(tf, gamma, l.g),
                  flux_tf_encode(tf, gamma, l.b));
}

float3 flux_tf_decode3(int tf, float gamma, float3 e) {
    return float3(flux_tf_decode(tf, gamma, e.r), flux_tf_decode(tf, gamma, e.g),
                  flux_tf_decode(tf, gamma, e.b));
}

/* Nominal reference white for ITU-R BT.2408 SDR graphics white in HDR (203 cd/m²). */
#define FLUX_SDR_WHITE_NITS_DEFAULT 203.0f

/* Decode straight encoded colour into the linear working space (where 1.0 = SDR white). */
float3 flux_decode_to_working(int tf, float gamma, float3 e) {
    float3 lin = flux_tf_decode3(tf, gamma, e);
    if (tf == FLUX_TF_PQ)
        lin *= (10000.0f / FLUX_SDR_WHITE_NITS_DEFAULT);
    else if (tf == FLUX_TF_HLG)
        lin *= (1000.0f / FLUX_SDR_WHITE_NITS_DEFAULT);
    return lin;
}

/* Premultiplied sRGB -> premultiplied linear (working space). */
float4 flux_decode_premul_srgb(float4 c) {
    float a = c.a;
    float3 straight = a > 0.0f ? c.rgb / a : float3(0.0f, 0.0f, 0.0f);
    return float4(flux_tf_decode3(FLUX_TF_SRGB, 0.0f, straight) * a, a);
}

/* Premultiplied linear (working space) -> premultiplied encoded. */
float4 flux_encode_premul(int tf, float gamma, float4 c) {
    float a = c.a;
    float3 straight = a > 0.0f ? c.rgb / a : float3(0.0f, 0.0f, 0.0f);
    return float4(flux_tf_encode3(tf, gamma, straight) * a, a);
}

/* HDR->SDR highlight rolloff (ADR-0069): working-space values above 1.0
 * are compressed through a shoulder, scaling by the max component so
 * hue survives. Identity in the SDR range. */
float3 flux_tonemap_shoulder(float3 c) {
    float m = max(c.r, max(c.g, c.b));
    if (m <= 1.0f)
        return c;
    float compressed = 1.0f + (m - 1.0f) / (1.0f + (m - 1.0f));
    return c * (compressed / m);
}

/* ITU-R BT.2390 EDR highlight rolloff: compresses values above knee up to
 * headroom with a smooth Hermite spline, preserving hue by scaling max component. */
float3 flux_tonemap_bt2390_shoulder(float3 c, float knee, float headroom) {
    if (headroom <= knee)
        return clamp(c, 0.0f, headroom);
    float m = max(c.r, max(c.g, c.b));
    if (m <= knee)
        return c;
    float t = clamp((m - knee) / (headroom - knee), 0.0f, 1.0f);
    float h = 3.0f * t * t - 2.0f * t * t * t;
    float compressed = lerp(knee, headroom, h);
    return c * (compressed / m);
}

/* Triangular-PDF dither bounded to ±0.45 LSB of `levels`, keyed on the fragment coordinate. */
float flux_dither_tpdf(float2 frag_coord, float levels) {
    float2 p = frac(frag_coord * float2(0.7548776662f, 0.5698402910f));
    float r = frac(p.x + p.y + p.x * p.y * 52.0f);
    float r2 = frac(r * 1.3247179572f);
    return (r + r2 - 1.0f) * (0.45f / levels);
}

#endif /* FLUX_COLOR_SLANG */
