#version 450
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

/*
 * Modern GPU 2D Analytic Fragment Vector Rasterizer (ADR-0116 ANALYTIC_FRAGMENT).
 *
 * Evaluates exact analytic vector coverage directly inside the fragment stage.
 * Consumes object/pixel-space line segments via 64-bit Buffer Device Address.
 * Zero CPU rasterization, zero texture bake, zero MSAA attachment, zero stencil.
 */

#include "canvas_color.glsl"

struct GpuSegment {
    float x0, y0;
    float x1, y1;
};

layout(buffer_reference, std430) readonly buffer SegmentsBuf {
    GpuSegment segs[];
};

struct GradStop {
    float t;
    uint color;
};

layout(buffer_reference, std430) readonly buffer VBUnused {
    uint unused;
};
layout(buffer_reference, std430) readonly buffer CPUnused {
    uint unused;
};

layout(push_constant) uniform PC {
    VBUnused verts;
    vec2 inv_window_size;
    CPUnused color_params;
    uint kind; /* 0 = solid, 1 = linear grad, 2 = radial grad */
    uint num_stops;
    vec2 grad_from;
    vec2 grad_to;
    float grad_radius;
    float _pad1;
    GradStop stops[8];
    SegmentsBuf segs;
    uint seg_count;
    uint fill_rule; /* 0 = NonZero, 1 = EvenOdd */
    uint _pad2;
}
pc;

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_pos;
layout(location = 2) in vec2 v_uv;

layout(location = 0) out vec4 out_color;

vec4 unpack_color(uint c) {
    vec4 packed_srgb = vec4(float((c >> 16) & 0xFFu), float((c >> 8) & 0xFFu),
                            float((c >> 0) & 0xFFu), float((c >> 24) & 0xFFu)) /
                       255.0;
    return flux_decode_premul_srgb(packed_srgb);
}

vec4 eval_grad(float t) {
    if (pc.num_stops == 0u)
        return v_color;
    if (t <= pc.stops[0].t)
        return unpack_color(pc.stops[0].color);
    if (t >= pc.stops[pc.num_stops - 1u].t)
        return unpack_color(pc.stops[pc.num_stops - 1u].color);

    for (uint i = 1u; i < pc.num_stops; ++i) {
        if (t <= pc.stops[i].t) {
            float t0 = pc.stops[i - 1u].t;
            float t1 = pc.stops[i].t;
            float f = (t - t0) / (t1 - t0 > 0.0 ? t1 - t0 : 1.0);
            vec4 c0 = unpack_color(pc.stops[i - 1u].color);
            vec4 c1 = unpack_color(pc.stops[i].color);
            return mix(c0, c1, f);
        }
    }
    return unpack_color(pc.stops[pc.num_stops - 1u].color);
}

void main() {
    vec2 p = v_pos;

    int winding = 0;
    float min_dist_sq = 1e30;

    for (uint i = 0u; i < pc.seg_count; ++i) {
        GpuSegment s = pc.segs.segs[i];
        float y0 = s.y0;
        float y1 = s.y1;
        float x0 = s.x0;
        float x1 = s.x1;

        if (y0 == y1)
            continue;

        float ymin = min(y0, y1);
        float ymax = max(y0, y1);

        // Ray casting in +X direction
        if (ymin <= p.y && p.y < ymax) {
            float t = (p.y - y0) / (y1 - y0);
            float x_int = mix(x0, x1, t);
            if (x_int > p.x) {
                winding += (y1 > y0) ? 1 : -1;
            }
        }

        // Distance to line segment in physical pixel space
        vec2 pa = p - vec2(x0, y0);
        vec2 ba = vec2(x1 - x0, y1 - y0);
        float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
        vec2 diff = pa - ba * h;
        float d2 = dot(diff, diff);
        if (d2 < min_dist_sq)
            min_dist_sq = d2;
    }

    bool inside = (pc.fill_rule == 1u) ? ((winding & 1) != 0) : (winding != 0);

    float dist = sqrt(min_dist_sq);
    float signed_dist = inside ? dist : -dist;

    // In 2D physical pixel space, exactly 1 pixel width = 1.0!
    float coverage = clamp(signed_dist + 0.5, 0.0, 1.0);

    if (coverage <= 0.0)
        discard;

    vec4 src_color;
    if (pc.kind == 0u) {
        src_color = v_color;
    } else if (pc.kind == 1u) {
        // Linear gradient
        vec2 d = pc.grad_to - pc.grad_from;
        float len2 = dot(d, d);
        float t = clamp(dot(p - pc.grad_from, d) / (len2 > 0.0 ? len2 : 1.0), 0.0, 1.0);
        src_color = eval_grad(t);
    } else {
        // Radial gradient
        float t = clamp(length(p - pc.grad_from) / pc.grad_radius, 0.0, 1.0);
        src_color = eval_grad(t);
    }

    out_color = vec4(src_color.rgb * coverage, src_color.a * coverage);
}
