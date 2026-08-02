#version 450
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 0) uniform texture2D u_textures[];
layout(set = 0, binding = 2) uniform sampler u_samplers[];

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer JointPalette {
    mat4 joints[];
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer UnlitParams {
    vec4 base_color;
    vec4 uv_scale_offset;
    vec4 uv_rotation_alpha_cutoff;
    uvec4 texture_info;
    JointPalette palette;
    uint joint_count;
};

layout(push_constant) uniform PC {
    mat4 mvp;
    UnlitParams params;
} pc;

layout(location = 0) in vec2 v_uv;
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
        base *= texture(sampler2D(u_textures[nonuniformEXT(ih)],
                                  u_samplers[nonuniformEXT(sh)]), uv);
    }

    uint alpha_mode = pc.params.texture_info.z;
    if (alpha_mode == 1u) {
        if (base.a < pc.params.uv_rotation_alpha_cutoff.z)
            discard;
        base.a = 1.0;
    } else if (alpha_mode == 0u) {
        base.a = 1.0;
    }
    out_color = base;
}
