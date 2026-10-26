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
#include "flux_vector_eval.glsl"

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

    int winding = 0;
    float min_dist_sq = 1e30;

    for (uint i = 0u; i < vp.seg_count; ++i) {
        GpuCurveSeg s = vp.segs.segs[i];
        flux_vector_eval_segment(p, vec2(s.x0, s.y0), vec2(s.x1, s.y1), winding, min_dist_sq);
    }

    float coverage = flux_vector_eval_coverage_jacobian(winding, min_dist_sq, vp.fill_rule, v_obj_pos);

    if (coverage <= 0.0)
        discard;

    vec4 base = vp.base_color;
    out_color = vec4(base.rgb * coverage, base.a * coverage);
}
