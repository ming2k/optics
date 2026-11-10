/*
 * canvas_vector.frag.slang — GPU 2D Analytic Fragment Vector Rasterizer in Slang (ADR-0116).
 */

#include "canvas_color.slang"
#include "flux_vector_eval.slang"

struct GpuSegment {
    float x0, y0;
    float x1, y1;
};

struct GradStop {
    float    t;
    uint32_t color;
};

struct PushConstants {
    uint64_t     verts;
    float2       inv_window_size;
    uint64_t     color_params;
    uint32_t     kind; /* 0 = solid, 1 = linear grad, 2 = radial grad */
    uint32_t     num_stops;
    float2       grad_from;
    float2       grad_to;
    float        grad_radius;
    float        _pad1;
    GradStop     stops[8];
    GpuSegment*  segs;
    uint32_t     seg_count;
    uint32_t     fill_rule; /* 0 = NonZero, 1 = EvenOdd */
    uint32_t     _pad2;
};

[[vk::push_constant]]
PushConstants pc;

struct FragmentInput {
    [[vk::location(0)]] float4 v_color : COLOR0;
    [[vk::location(1)]] float2 v_pos   : POSITION0;
    [[vk::location(2)]] float2 v_uv    : TEXCOORD0;
};

float4 unpack_color(uint32_t c) {
    float4 packed_srgb = float4(float((c >> 16) & 0xFFu), float((c >> 8) & 0xFFu),
                                float((c >> 0) & 0xFFu), float((c >> 24) & 0xFFu)) /
                         255.0f;
    return flux_decode_premul_srgb(packed_srgb);
}

float4 eval_grad(float t, float4 v_color) {
    if (pc.num_stops == 0u)
        return v_color;
    if (t <= pc.stops[0].t)
        return unpack_color(pc.stops[0].color);
    if (t >= pc.stops[pc.num_stops - 1u].t)
        return unpack_color(pc.stops[pc.num_stops - 1u].color);

    for (uint32_t i = 1u; i < pc.num_stops; ++i) {
        if (t <= pc.stops[i].t) {
            float t0 = pc.stops[i - 1u].t;
            float t1 = pc.stops[i].t;
            float f = (t - t0) / (t1 - t0 > 0.0f ? t1 - t0 : 1.0f);
            float4 c0 = unpack_color(pc.stops[i - 1u].color);
            float4 c1 = unpack_color(pc.stops[i].color);
            return lerp(c0, c1, f);
        }
    }
    return unpack_color(pc.stops[pc.num_stops - 1u].color);
}

[shader("fragment")]
float4 main(FragmentInput input) : SV_Target {
    float2 p = input.v_pos;

    int winding = 0;
    float min_dist_sq = 1e30f;

    for (uint32_t i = 0u; i < pc.seg_count; ++i) {
        GpuSegment s = pc.segs[i];
        flux_vector_eval_segment(p, float2(s.x0, s.y0), float2(s.x1, s.y1), winding, min_dist_sq);
    }

    float coverage = flux_vector_eval_coverage(winding, min_dist_sq, pc.fill_rule, 1.0f);

    if (coverage <= 0.0f)
        discard;

    float4 src_color;
    if (pc.kind == 0u) {
        src_color = input.v_color;
    } else if (pc.kind == 1u) {
        float2 d = pc.grad_to - pc.grad_from;
        float len2 = dot(d, d);
        float t = clamp(dot(p - pc.grad_from, d) / (len2 > 0.0f ? len2 : 1.0f), 0.0f, 1.0f);
        src_color = eval_grad(t, input.v_color);
    } else {
        float t = clamp(length(p - pc.grad_from) / pc.grad_radius, 0.0f, 1.0f);
        src_color = eval_grad(t, input.v_color);
    }

    return float4(src_color.rgb * coverage, src_color.a * coverage);
}
