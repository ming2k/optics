/*
 * canvas_gradient.frag.slang — Canvas gradient fragment shader in Slang.
 */

#include "canvas_color.slang"

struct GradStop {
    float    t;
    uint32_t color;
};

struct PushConstants {
    uint64_t verts;
    float2   inv_window_size;
    float2   _pad0;
    uint32_t kind;
    uint32_t num_stops;
    float2   grad_from;
    float2   grad_to;
    float    grad_radius;
    float    _pad1;
    GradStop stops[8];
};

[[vk::push_constant]]
PushConstants pc;

struct FragmentInput {
    [[vk::location(0)]] float4 v_color : COLOR0;
    [[vk::location(1)]] float2 v_pos   : POSITION0;
};

float4 unpack_color(uint32_t c)
{
    float4 packed_srgb = float4(
        float((c >> 16) & 0xFFu),
        float((c >>  8) & 0xFFu),
        float((c >>  0) & 0xFFu),
        float((c >> 24) & 0xFFu)
    ) / 255.0f;
    return flux_decode_premul_srgb(packed_srgb);
}

[shader("fragment")]
float4 main(FragmentInput input) : SV_Target
{
    float t = 0.0f;
    if (pc.kind == 1u) {
        float2 axis = pc.grad_to - pc.grad_from;
        float len2 = dot(axis, axis);
        if (len2 > 0.0f) {
            t = dot(input.v_pos - pc.grad_from, axis) / len2;
        }
    } else if (pc.kind == 2u) {
        if (pc.grad_radius > 0.0f) {
            t = length(input.v_pos - pc.grad_from) / pc.grad_radius;
        }
    }
    t = clamp(t, 0.0f, 1.0f);

    uint32_t n = pc.num_stops;
    if (n == 0u) {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    if (t <= pc.stops[0].t) {
        return unpack_color(pc.stops[0].color);
    }
    if (t >= pc.stops[n - 1u].t) {
        return unpack_color(pc.stops[n - 1u].color);
    }
    for (uint32_t i = 1u; i < n; ++i) {
        float t1 = pc.stops[i].t;
        if (t <= t1) {
            float t0 = pc.stops[i - 1u].t;
            float u  = (t1 > t0) ? (t - t0) / (t1 - t0) : 0.0f;
            float4 c0 = unpack_color(pc.stops[i - 1u].color);
            float4 c1 = unpack_color(pc.stops[i].color);
            return lerp(c0, c1, u);
        }
    }
    return unpack_color(pc.stops[n - 1u].color);
}
