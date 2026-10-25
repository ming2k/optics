#version 450
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

/*
 * Scene vector coverage fragment shader (ADR-0117).
 *
 * Evaluates exact analytic vector coverage in screen space under arbitrary
 * 3D perspective transforms via screen Jacobian derivatives.
 * Strictly satisfies INV-SCV-01 (no bake) and INV-SCV-03 (projective correctness).
 */

#include "scene_color.glsl"

layout(push_constant) uniform PC {
    mat4 mvp;
    VectorParams params;
}
pc;

layout(location = 0) in vec2 v_obj_pos;
layout(location = 1) in vec2 v_uv;

layout(location = 0) out vec4 out_color;

void main() {
    VectorParams vp = pc.params;
    vec2 p = v_obj_pos;

    // 1. Ray-cast in object space along +X
    int winding = 0;
    float min_dist_sq = 1e30;

    for (uint i = 0u; i < vp.seg_count; ++i) {
        GpuCurveSeg s = vp.segs.segs[i];
        float y0 = s.y0;
        float y1 = s.y1;
        float x0 = s.x0;
        float x1 = s.x1;

        if (y0 == y1)
            continue;

        float ymin = min(y0, y1);
        float ymax = max(y0, y1);

        if (ymin <= p.y && p.y < ymax) {
            float t = (p.y - y0) / (y1 - y0);
            float x_int = mix(x0, x1, t);
            if (x_int > p.x) {
                winding += (y1 > y0) ? 1 : -1;
            }
        }

        // Distance to segment in object space
        vec2 pa = p - vec2(x0, y0);
        vec2 ba = vec2(x1 - x0, y1 - y0);
        float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
        vec2 diff = pa - ba * h;
        float d2 = dot(diff, diff);
        if (d2 < min_dist_sq)
            min_dist_sq = d2;
    }

    bool inside = (vp.fill_rule == 1u) ? ((winding & 1) != 0) : (winding != 0);

    // 2. Screen-space Jacobian: derivatives of object-space coordinates w.r.t pixels
    vec2 dpos_dx = dFdx(v_obj_pos);
    vec2 dpos_dy = dFdy(v_obj_pos);
    float pixel_scale = sqrt(max(dot(dpos_dx, dpos_dx), dot(dpos_dy, dpos_dy)));
    if (pixel_scale <= 1e-7)
        pixel_scale = 1e-7;

    float dist = sqrt(min_dist_sq);
    float signed_dist = inside ? dist : -dist;

    // 3. Analytic screen-space coverage ramp (0.5 pixel transition)
    float coverage = clamp(signed_dist / pixel_scale + 0.5, 0.0, 1.0);

    if (coverage <= 0.0)
        discard;

    vec4 base = vp.base_color;
    out_color = vec4(base.rgb * coverage, base.a * coverage);
}
