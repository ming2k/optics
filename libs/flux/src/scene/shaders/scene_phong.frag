#version 450
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

#include "scene_color.glsl"

layout(push_constant) uniform PC {
    mat4 mvp;
    PhongParams params;
} pc;

layout(location = 0) in vec3 v_world_pos;
layout(location = 1) in vec3 v_world_nrm;
layout(location = 2) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

void main()
{
    vec2 scaled = v_uv * pc.params.uv_scale_offset.xy;
    float c = pc.params.uv_rotation_alpha_cutoff.x;
    float s = pc.params.uv_rotation_alpha_cutoff.y;
    vec2 uv = vec2(c * scaled.x - s * scaled.y,
                   s * scaled.x + c * scaled.y)
            + pc.params.uv_scale_offset.zw;

    vec4 base = pc.params.base_color;
    if (pc.params.texture_info.w != 0u) {
        uint ih = pc.params.texture_info.x & 0x0FFFFFFFu;
        uint sh = pc.params.texture_info.y & 0x0FFFFFFFu;
        vec4 texel = texture(sampler2D(u_textures[nonuniformEXT(ih)],
                                       u_samplers[nonuniformEXT(sh)]), uv);
        /* ADR-0069: rendering into the linear working space (16F target)
         * decodes texels at the edge, so lighting runs in linear light;
         * legacy 8-bit targets stay raw. */
        if ((pc.params.color_flags & SCENE_COLOR_DECODE) != 0u)
            texel.rgb = (pc.params.color_flags & SCENE_COLOR_HAS_PARAMS) != 0u
                            ? scene_decode_tagged(pc.params.color_params.cp, sh, texel.rgb)
                            : flux_tf_decode3(FLUX_TF_SRGB, 0.0, texel.rgb);
        base *= texel;
    }
    uint alpha_mode = pc.params.texture_info.z;
    if (alpha_mode == 1u) {
        if (base.a < pc.params.uv_rotation_alpha_cutoff.z)
            discard;
        base.a = 1.0;
    } else if (alpha_mode == 0u) {
        base.a = 1.0;
    }

    vec3 N = normalize(v_world_nrm);
    if (!gl_FrontFacing)
        N = -N;
    vec3 L = normalize(-pc.params.light_dir_shininess.xyz);
    float ndotl = max(dot(N, L), 0.0);
    vec3 V = normalize(pc.params.eye_specular.xyz - v_world_pos);
    vec3 H = normalize(L + V);
    float spec = ndotl > 0.0
        ? pow(max(dot(N, H), 0.0), pc.params.light_dir_shininess.w)
        : 0.0;
    vec3 light_color = pc.params.light_color_ambient.rgb;
    vec3 color = base.rgb * pc.params.light_color_ambient.w * light_color
               + base.rgb * ndotl * light_color
               + pc.params.eye_specular.w * spec * light_color;
    out_color = vec4(color, base.a);
}
