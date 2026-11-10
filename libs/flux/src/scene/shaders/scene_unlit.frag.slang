/*
 * scene_unlit.frag.slang — Unlit scene fragment shader in Slang.
 */

#include "scene_color.slang"

struct PushConstants {
    float4x4     mvp;
    UnlitParams* params;
};

[[vk::push_constant]]
PushConstants pc;

struct FragmentInput {
    [[vk::location(0)]] float2 v_uv : TEXCOORD0;
};

[shader("fragment")]
float4 main(FragmentInput input) : SV_Target
{
    float2 scaled = input.v_uv * pc.params.uv_scale_offset.xy;
    float c = pc.params.uv_rotation_alpha_cutoff.x;
    float s = pc.params.uv_rotation_alpha_cutoff.y;
    float2 uv = float2(c * scaled.x - s * scaled.y,
                       s * scaled.x + c * scaled.y)
              + pc.params.uv_scale_offset.zw;

    float4 base_col = pc.params.base_color;
    if (pc.params.texture_info.w != 0u) {
        uint32_t ih = pc.params.texture_info.x & 0x0FFFFFFFu;
        uint32_t sh = pc.params.texture_info.y & 0x0FFFFFFFu;
        float4 texel = u_textures[NonUniformResourceIndex(ih)].Sample(
            u_samplers[NonUniformResourceIndex(sh)], uv);

        if ((pc.params.color_flags & SCENE_COLOR_DECODE) != 0u) {
            texel.rgb = (pc.params.color_flags & SCENE_COLOR_HAS_PARAMS) != 0u
                            ? scene_decode_tagged(*pc.params.color_params, sh, texel.rgb)
                            : flux_tf_decode3(FLUX_TF_SRGB, 0.0f, texel.rgb);
        }
        base_col *= texel;
    }

    uint32_t alpha_mode = pc.params.texture_info.z;
    if (alpha_mode == 1u) {
        if (base_col.a < pc.params.uv_rotation_alpha_cutoff.z)
            discard;
        base_col.a = 1.0f;
    } else if (alpha_mode == 0u) {
        base_col.a = 1.0f;
    }

    return base_col;
}
