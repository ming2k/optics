#version 460

/* Hard-coded NDC triangle. Used by test_graphics_pipeline to verify
 * pipeline creation succeeds end-to-end (shader → layout → pipeline).
 * Identical layout to examples/shaders/triangle.vert so the test
 * mirrors how a real user would build one. */

vec2 positions[3] = vec2[](
    vec2( 0.0, -0.5),
    vec2( 0.5,  0.5),
    vec2(-0.5,  0.5)
);

vec3 colors[3] = vec3[](
    vec3(1.0, 0.0, 0.0),
    vec3(0.0, 1.0, 0.0),
    vec3(0.0, 0.0, 1.0)
);

layout(location = 0) out vec3 v_color;

void main()
{
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    v_color     = colors[gl_VertexIndex];
}
