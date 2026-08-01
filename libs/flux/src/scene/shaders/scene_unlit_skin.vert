#version 450
#extension GL_EXT_buffer_reference : enable

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in uvec4 a_joints;
layout(location = 4) in vec4 a_weights;

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer JointPalette {
    mat4 joints[];
};

layout(push_constant) uniform PC {
    mat4 mvp;
    vec4 color;
    JointPalette palette;
    uint joint_count;
} pc;

layout(location = 0) out vec4 v_color;

void main()
{
    vec4 weights = max(a_weights, vec4(0.0));
    float total = dot(weights, vec4(1.0));
    weights = total > 0.0 ? weights / total : vec4(1.0, 0.0, 0.0, 0.0);
    uvec4 joints = min(a_joints, uvec4(max(pc.joint_count, 1u) - 1u));
    mat4 skin = weights.x * pc.palette.joints[joints.x]
              + weights.y * pc.palette.joints[joints.y]
              + weights.z * pc.palette.joints[joints.z]
              + weights.w * pc.palette.joints[joints.w];
    gl_Position = pc.mvp * skin * vec4(a_position, 1.0);
    v_color = pc.color;
}
