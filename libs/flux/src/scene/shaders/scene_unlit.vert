#version 450

/*
 * Unlit scene vertex shader.
 *
 * Vertex format matches flux_vertex (position vec3, normal vec3,
 * uv vec2 — 32 bytes per vertex). Normal and uv are accepted for
 * compatibility with the mesh format but unused by unlit.
 */

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;   /* unused for unlit */
layout(location = 2) in vec2 a_uv;       /* unused for unlit */

layout(push_constant) uniform PC {
    mat4 mvp;
    vec4 color;
} pc;

layout(location = 0) out vec4 v_color;

void main()
{
    gl_Position = pc.mvp * vec4(a_position, 1.0);
    v_color     = pc.color;
}
