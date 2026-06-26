#version 450

/* Solid-colour canvas fragment shader.
 * Vertex colour wins; pixel position varying is ignored. */

layout(location = 0) in  vec4 v_color;
layout(location = 1) in  vec2 v_pos;
layout(location = 0) out vec4 out_color;

void main()
{
    out_color = v_color;
}
