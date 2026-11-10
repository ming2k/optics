/*
 * scene_vector.frag.slang — Scene vector coverage fragment shader in Slang (ADR-0117).
 */

#include "scene_color.slang"
#include "flux_vector_eval.slang"

struct PushConstants {
    float4x4      mvp;
    VectorParams* params;
};

[[vk::push_constant]]
PushConstants pc;

struct FragmentInput {
    [[vk::location(0)]] float2 v_obj_pos : POSITION0;
    [[vk::location(1)]] float2 v_uv      : TEXCOORD0;
};

[shader("fragment")]
float4 main(FragmentInput input) : SV_Target {
    VectorParams vp = *pc.params;
    float2 p = input.v_obj_pos;

    int winding = 0;
    float min_dist_sq = 1e30f;

    for (uint32_t i = 0u; i < vp.seg_count; ++i) {
        GpuCurveSeg s = vp.segs[i];
        flux_vector_eval_segment(p, float2(s.x0, s.y0), float2(s.x1, s.y1), winding, min_dist_sq);
    }

    float coverage = flux_vector_eval_coverage_jacobian(winding, min_dist_sq, vp.fill_rule, input.v_obj_pos);

    if (coverage <= 0.0f)
        discard;

    float4 base_col = vp.base_color;
    return float4(base_col.rgb * coverage, base_col.a * coverage);
}
