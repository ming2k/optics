/*
 * canvas_sdf.frag.slang — Signed-distance-field fragment shader in Slang (ADR-0118 / ADR-0148).
 */

#include "flux_sdf.slang"

struct PushConstants {
    [[vk::offset(32)]]  float4 sdf_inv;    /* inverse linear: a, b, c, d (row-major) */
    [[vk::offset(128)]] float4 sdf_box;    /* cx, cy, hx, hy */
    [[vk::offset(144)]] float4 sdf_param;  /* radius, stroke_hw, curvature, aa */
};

[[vk::push_constant]]
PushConstants pc;

struct FragmentInput {
    [[vk::location(0)]] float4 v_color : COLOR0;
    [[vk::location(1)]] float2 v_pos   : POSITION0;
};

[shader("fragment")]
float4 main(FragmentInput input) : SV_Target
{
    float2 rel = input.v_pos - pc.sdf_box.xy;
    float2 p   = float2(pc.sdf_inv.x * rel.x + pc.sdf_inv.y * rel.y,
                        pc.sdf_inv.z * rel.x + pc.sdf_inv.w * rel.y);

    float d = flux_sdf_squircle(p, pc.sdf_box.zw, pc.sdf_param.x, pc.sdf_param.z);
    d = flux_sdf_ring(d, pc.sdf_param.y);

    float aa  = max(pc.sdf_param.w, 1e-4f);
    float cov = clamp(0.5f - d / aa, 0.0f, 1.0f);

    return input.v_color * cov;
}
