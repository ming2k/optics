#version 450
#extension GL_EXT_buffer_reference : require

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;

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

layout(location = 0) out vec2 v_uv;

void main()
{
    gl_Position = pc.mvp * vec4(a_position, 1.0);
    v_uv = a_uv;
}
