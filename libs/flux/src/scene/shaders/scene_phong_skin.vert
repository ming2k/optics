#version 450
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

#include "scene_color.glsl"

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in uvec4 a_joints;
layout(location = 4) in vec4 a_weights;

layout(push_constant) uniform PC {
    mat4 mvp;
    PhongParams params;
} pc;

layout(location = 0) out vec3 v_world_pos;
layout(location = 1) out vec3 v_world_nrm;
layout(location = 2) out vec2 v_uv;

void main()
{
    vec4 weights = max(a_weights, vec4(0.0));
    float total = dot(weights, vec4(1.0));
    weights = total > 0.0 ? weights / total : vec4(1.0, 0.0, 0.0, 0.0);
    uvec4 joints = min(a_joints, uvec4(max(pc.params.joint_count, 1u) - 1u));
    mat4 skin = weights.x * pc.params.palette.joints[joints.x]
              + weights.y * pc.params.palette.joints[joints.y]
              + weights.z * pc.params.palette.joints[joints.z]
              + weights.w * pc.params.palette.joints[joints.w];
    vec4 skinned_position = skin * vec4(a_position, 1.0);
    vec3 skinned_normal = mat3(skin) * a_normal;
    gl_Position = pc.mvp * skinned_position;
    v_world_pos = (pc.params.world * skinned_position).xyz;
    mat3 nrm = mat3(pc.params.nrm0.xyz, pc.params.nrm1.xyz, pc.params.nrm2.xyz);
    v_world_nrm = nrm * skinned_normal;
    v_uv = a_uv;
}
