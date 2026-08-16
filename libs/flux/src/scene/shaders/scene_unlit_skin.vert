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
    UnlitParams params;
} pc;

layout(location = 0) out vec2 v_uv;

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
    gl_Position = pc.mvp * skin * vec4(a_position, 1.0);
    v_uv = a_uv;
}
