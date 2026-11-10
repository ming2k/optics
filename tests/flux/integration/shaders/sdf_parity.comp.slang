/*
 * sdf_parity.comp.slang — differential harness for the SDF derivative (ADR-0148) in Slang.
 *
 * Evaluates `flux_sdf.slang` on the GPU and writes its distance and normal into a storage
 * buffer, so test_sdf_parity can compare them against <flux/sdf.h> on the CPU.
 */

#include "flux_sdf.slang"

struct Params {
    uint64_t out_address;
    float2   half_extent;
    float    radius;
    float    curvature;
    float    aa;
    uint32_t sample_count;
    uint32_t mode;
    float    pad1;
    float    pad2;
};

[[vk::push_constant]]
Params pc;

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint32_t i = dispatchThreadID.x;
    if (i >= pc.sample_count) return;

    uint32_t cols = uint32_t(ceil(sqrt(float(pc.sample_count))));
    uint32_t rows = (pc.sample_count + cols - 1u) / cols;
    float u = (rows <= 1u) ? 0.5f : (float(i / cols) / float(rows - 1u));
    float v = (cols <= 1u) ? 0.5f : (float(i % cols) / float(cols - 1u));

    float2 half_extent = pc.half_extent;
    float extent = max(half_extent.x, half_extent.y);
    float2 p = (float2(v, u) * 2.0f - 1.0f) * extent * 2.2f;

    float3 s;
    if (pc.mode == 2u) {
        float2 p_b = p - float2(half_extent.x * 0.75f, 0.0f);
        float3 a = flux_sdf_squircle_sample(p, half_extent, pc.radius * 0.25f, pc.curvature, pc.aa);
        float3 b_ = flux_sdf_squircle_sample(p_b, half_extent * 0.7f, pc.radius * 0.25f, pc.curvature, pc.aa);
        float h;
        float d = flux_smooth_union(float2(a.x, b_.x), pc.radius, h);
        float2 g = flux_smooth_union_gradient(a.yz, b_.yz, float2(a.x, b_.x), pc.radius);
        s = float3(d, g.x, g.y);
    } else if (pc.mode == 1u) {
        float d = flux_sdf_squircle(p, half_extent, pc.radius, pc.curvature);
        float2 g = flux_sdf_squircle_gradient(p, half_extent, pc.radius, pc.curvature);
        s = float3(d, g.x, g.y);
    } else {
        s = flux_sdf_squircle_sample(p, half_extent, pc.radius, pc.curvature, pc.aa);
    }

    float4* dst = (float4*)pc.out_address;
    dst[i] = float4(s.x, s.y, s.z, 0.0f);
}
