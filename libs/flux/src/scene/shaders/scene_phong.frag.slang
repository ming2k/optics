/*
 * scene_phong.frag.slang — Phong scene fragment shader in Slang.
 */

#include "scene_color.slang"

struct PushConstants {
    float4x4     mvp;
    PhongParams* params;
};

[[vk::push_constant]]
PushConstants pc;

struct FragmentInput {
    [[vk::location(0)]] float3 v_world_pos : POSITION0;
    [[vk::location(1)]] float3 v_world_nrm : NORMAL0;
    [[vk::location(2)]] float2 v_uv        : TEXCOORD0;
    bool is_front_face : SV_IsFrontFace;
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

    float3 N = normalize(input.v_world_nrm);
    if (!input.is_front_face)
        N = -N;
    float3 L = normalize(-pc.params.light_dir_shininess.xyz);
    float ndotl = max(dot(N, L), 0.0f);
    float3 V = normalize(pc.params.eye_specular.xyz - input.v_world_pos);
    float3 H = normalize(L + V);
    float spec = ndotl > 0.0f
        ? pow(max(dot(N, H), 0.0f), pc.params.light_dir_shininess.w)
        : 0.0f;

    float3 diffuse = base_col.rgb * pc.params.light_color_ambient.xyz * ndotl;
    float3 ambient = base_col.rgb * pc.params.light_color_ambient.w;
    float3 specular = pc.params.light_color_ambient.xyz * spec * pc.params.eye_specular.w;
    return float4(diffuse + ambient + specular, base_col.a);
}
