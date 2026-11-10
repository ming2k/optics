/*
 * canvas_output.frag.slang — Canvas output-transform fragment shader in Slang (ADR-0069).
 */

#include "canvas_color.slang"

[[vk::binding(0, 0)]] Texture2D u_textures[];
[[vk::binding(2, 0)]] SamplerState u_samplers[];

#define FLUX_OUTPUT_F_DECODE    0x1u
#define FLUX_OUTPUT_F_NO_DITHER 0x2u

struct PushConstants {
    float4   primaries_rows[3];
    uint32_t image_handle;
    uint32_t sampler_handle;
    uint32_t transfer;
    uint32_t flags;
    float    gamma;
    float    dither_levels;
    float    sdr_white_nits;
    float    uv_scale_u;
    float    uv_scale_v;
};

[[vk::push_constant]]
PushConstants pc;

struct FragmentInput {
    float4 sv_position : SV_Position;
    [[vk::location(0)]] float2 v_uv : TEXCOORD0;
};

[shader("fragment")]
float4 main(FragmentInput input) : SV_Target
{
    uint32_t ih = pc.image_handle   & 0x0FFFFFFFu;
    uint32_t sh = pc.sampler_handle & 0x0FFFFFFFu;
    float2 uv = clamp(input.v_uv, float2(0.0f, 0.0f), float2(1.0f, 1.0f)) * float2(pc.uv_scale_u, pc.uv_scale_v);
    float4 c = u_textures[NonUniformResourceIndex(ih)].Sample(u_samplers[NonUniformResourceIndex(sh)], uv);

    float a = c.a;
    float3 straight = a > 0.0f ? c.rgb / a : float3(0.0f, 0.0f, 0.0f);

    float sdr_white = pc.sdr_white_nits > 0.0f ? pc.sdr_white_nits : FLUX_SDR_WHITE_NITS_DEFAULT;
    if ((pc.flags & FLUX_OUTPUT_F_DECODE) != 0u) {
        float3 lin = flux_tf_decode3(int(pc.transfer), pc.gamma, straight);
        if (pc.transfer == FLUX_TF_PQ)
            lin *= (10000.0f / sdr_white);
        else if (pc.transfer == FLUX_TF_HLG)
            lin *= (1000.0f / sdr_white);
        float3 dec_lin = pc.primaries_rows[0].xyz * lin.x +
                         pc.primaries_rows[1].xyz * lin.y +
                         pc.primaries_rows[2].xyz * lin.z;
        return float4(dec_lin * a, a);
    }

    float3 linear_col = pc.primaries_rows[0].xyz * straight.x +
                        pc.primaries_rows[1].xyz * straight.y +
                        pc.primaries_rows[2].xyz * straight.z;
    if (pc.transfer == FLUX_TF_PQ) {
        float3 nits = linear_col * sdr_white;
        float3 rolled = flux_tonemap_bt2390_shoulder(nits, 1000.0f, 10000.0f);
        linear_col = clamp(rolled / 10000.0f, 0.0f, 1.0f);
    } else if (pc.transfer == FLUX_TF_HLG) {
        float3 nits = linear_col * sdr_white;
        float3 rolled = flux_tonemap_bt2390_shoulder(nits, 300.0f, 1000.0f);
        linear_col = clamp(rolled / 1000.0f, 0.0f, 1.0f);
    } else if (pc.transfer != FLUX_TF_LINEAR) {
        linear_col = flux_tonemap_shoulder(linear_col);
    }
    float3 encoded = flux_tf_encode3(int(pc.transfer), pc.gamma, linear_col);
    float3 rgb = encoded * a;
    if ((pc.flags & FLUX_OUTPUT_F_NO_DITHER) == 0u)
        rgb += flux_dither_tpdf(input.sv_position.xy, pc.dither_levels);
    return float4(rgb, a);
}
