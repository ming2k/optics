#version 450
#extension GL_EXT_buffer_reference : require

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;

layout(buffer_reference, std430) readonly buffer PhongParams {
    mat4 world;
    vec4 nrm0;
    vec4 nrm1;
    vec4 nrm2;
    vec4 base_color;
    vec4 uv_scale_offset;
    vec4 uv_rotation_alpha_cutoff;
    uvec4 texture_info;
    vec4 light_dir_shininess;
    vec4 light_color_ambient;
    vec4 eye_specular;
};

layout(push_constant) uniform PC {
    mat4 mvp;
    PhongParams params;
} pc;

layout(location = 0) out vec3 v_world_pos;
layout(location = 1) out vec3 v_world_nrm;
layout(location = 2) out vec2 v_uv;

void main()
{
    gl_Position = pc.mvp * vec4(a_position, 1.0);
    v_world_pos = (pc.params.world * vec4(a_position, 1.0)).xyz;
    mat3 nrm = mat3(pc.params.nrm0.xyz, pc.params.nrm1.xyz, pc.params.nrm2.xyz);
    v_world_nrm = nrm * a_normal;
    v_uv = a_uv;
}
