#version 450

/*
 * Canvas output-transform vertex shader (ADR-0069).
 *
 * Fullscreen triangle from gl_VertexIndex — no vertex input, no
 * push constants. NDC (-1,-1) is the framebuffer's top-left and maps
 * to UV (0,0), matching how the canvas wrote the intermediate.
 */

layout(location = 0) out vec2 v_uv;

void main()
{
    vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
    v_uv = pos;
}
