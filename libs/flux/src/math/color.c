/*
 * Colour — packed 0xAARRGGBB, premultiplied alpha.
 *
 * to_linear / from_linear round-trip through sRGB. The packed colour
 * is premultiplied in sRGB space; conversion undoes the premul,
 * linearises, then re-premultiplies in linear space.
 */
#include <flux/math.h>
#include <math.h>

flux_color flux_color_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

flux_color flux_color_rgba_premul(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (a == 255)
        return flux_color_rgba(r, g, b, a);
    if (a == 0)
        return 0;
    uint32_t af = a;
    uint8_t pr = (uint8_t)((r * af + 127) / 255);
    uint8_t pg = (uint8_t)((g * af + 127) / 255);
    uint8_t pb = (uint8_t)((b * af + 127) / 255);
    return flux_color_rgba(pr, pg, pb, a);
}

void flux_color_unpack(flux_color c, uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *a) {
    if (a)
        *a = (uint8_t)((c >> 24) & 0xFF);
    if (r)
        *r = (uint8_t)((c >> 16) & 0xFF);
    if (g)
        *g = (uint8_t)((c >> 8) & 0xFF);
    if (b)
        *b = (uint8_t)(c & 0xFF);
}

static float srgb_to_linear(float c) {
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static float linear_to_srgb(float c) {
    if (c <= 0.0f)
        return 0.0f;
    if (c >= 1.0f)
        return 1.0f;
    return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

flux_vec4 flux_color_to_linear(flux_color c) {
    uint8_t r, g, b, a;
    flux_color_unpack(c, &r, &g, &b, &a);
    float af = (float)a / 255.0f;
    float inv_a = af > 0.0f ? 1.0f / af : 0.0f;
    float rf = (float)r / 255.0f * inv_a;
    float gf = (float)g / 255.0f * inv_a;
    float bf = (float)b / 255.0f * inv_a;
    return (flux_vec4){
        srgb_to_linear(rf) * af,
        srgb_to_linear(gf) * af,
        srgb_to_linear(bf) * af,
        af,
    };
}

static uint8_t clamp_u8(float f) {
    if (f <= 0.0f)
        return 0;
    if (f >= 1.0f)
        return 255;
    return (uint8_t)lrintf(f * 255.0f);
}

flux_color flux_color_from_linear(flux_vec4 linear) {
    float inv_a = linear.w > 0.0f ? 1.0f / linear.w : 0.0f;
    uint8_t r = clamp_u8(linear_to_srgb(linear.x * inv_a) * linear.w);
    uint8_t g = clamp_u8(linear_to_srgb(linear.y * inv_a) * linear.w);
    uint8_t b = clamp_u8(linear_to_srgb(linear.z * inv_a) * linear.w);
    uint8_t a = clamp_u8(linear.w);
    return flux_color_rgba_premul(r, g, b, a);
}
