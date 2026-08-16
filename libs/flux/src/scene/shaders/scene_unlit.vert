#version 450
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

#include "scene_color.glsl"

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;

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
