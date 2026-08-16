#version 450
#extension GL_GOOGLE_include_directive : require

/*
 * Canvas gradient fragment shader.
 *
 * Computes a gradient parameter t per fragment and linearly
 * interpolates between two adjacent stops. Stops live in push
 * constants (struct layout matches flux_canvas_push in
 * src/canvas/internal.h). Colours are stored as packed 0xAARRGGBB
 * premultiplied sRGB and decoded to premultiplied linear at unpack
 * (ADR-0069), so stop interpolation itself happens in linear light.
 *
 * kind == 1 (LINEAR):
 *   t = clamp(dot(p - from, to - from) / |to - from|^2, 0, 1)
 *
 * kind == 2 (RADIAL):
 *   t = clamp(|p - centre| / radius, 0, 1)
 */

#include "canvas_color.glsl"

layout(location = 0) in  vec4 v_color;   /* unused — gradient determines colour */
layout(location = 1) in  vec2 v_pos;
layout(location = 0) out vec4 out_color;

#extension GL_EXT_buffer_reference : enable
struct VertexUnused {
    vec2 pos;
    uint color;
    uint _pad;
};
layout(buffer_reference, std430) readonly buffer VBUnused { VertexUnused v[]; };

struct GradStop {
    float    t;
    uint     color;
    /* std430 packs the 8-byte struct; no manual padding needed */
};

layout(push_constant) uniform PC {
    VBUnused verts;                /* offset  0  (16B aligned for ptr) */
    vec2     inv_window_size;      /* offset  8 */
    vec2     _pad0;                /* offset 16 */
    uint     kind;                 /* offset 24 */
    uint     num_stops;            /* offset 28 */
    vec2     grad_from;            /* offset 32 */
    vec2     grad_to;              /* offset 40 */
    float    grad_radius;          /* offset 48 */
    float    _pad1;                /* offset 52 */
    GradStop stops[8];             /* offset 56, size 64 */
} pc;

vec4 unpack_color(uint c)
{
    return flux_decode_premul_srgb(vec4(
        float((c >> 16) & 0xFFu),
        float((c >>  8) & 0xFFu),
        float((c >>  0) & 0xFFu),
        float((c >> 24) & 0xFFu)
    ) / 255.0);
}

void main()
{
    float t = 0.0;
    if (pc.kind == 1u) {
        vec2 axis = pc.grad_to - pc.grad_from;
        float len2 = dot(axis, axis);
        if (len2 > 0.0) {
            t = dot(v_pos - pc.grad_from, axis) / len2;
        }
    } else if (pc.kind == 2u) {
        if (pc.grad_radius > 0.0) {
            t = length(v_pos - pc.grad_from) / pc.grad_radius;
        }
    }
    t = clamp(t, 0.0, 1.0);

    /* Binary fallback: linear search over stops (max 8 — cheap). */
    uint n = pc.num_stops;
    if (n == 0u) {
        out_color = vec4(0.0);
        return;
    }
    if (t <= pc.stops[0].t) {
        out_color = unpack_color(pc.stops[0].color);
        return;
    }
    if (t >= pc.stops[n - 1u].t) {
        out_color = unpack_color(pc.stops[n - 1u].color);
        return;
    }
    for (uint i = 1u; i < n; ++i) {
        float t1 = pc.stops[i].t;
        if (t <= t1) {
            float t0 = pc.stops[i - 1u].t;
            float u  = (t1 > t0) ? (t - t0) / (t1 - t0) : 0.0;
            vec4 c0 = unpack_color(pc.stops[i - 1u].color);
            vec4 c1 = unpack_color(pc.stops[i].color);
            out_color = mix(c0, c1, u);
            return;
        }
    }
    out_color = unpack_color(pc.stops[n - 1u].color);
}
