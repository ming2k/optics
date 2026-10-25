#version 450
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

/*
 * Scene vector coverage vertex shader (ADR-0117).
 *
 * Carries object-space coordinates to the fragment shader.
 * Does NOT interpolate control points per vertex (INV-SCV-02).
 */

#include "scene_color.glsl"

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;

layout(push_constant) uniform PC {
    mat4 mvp;
    VectorParams params;
}
pc;

layout(location = 0) out vec2 v_obj_pos;
layout(location = 1) out vec2 v_uv;

void main() {
    gl_Position = pc.mvp * vec4(a_position, 1.0);
    v_obj_pos = a_position.xy;
    v_uv = a_uv;
}
