#version 460

/* Vertex-buffer variant of triangle.vert: positions arrive through a
 * vertex attribute so a draw provably reads the bound VkBuffer. Used by
 * test_buffer_retire to reference a flux_buffer from an in-flight frame. */

layout(location = 0) in vec2 in_pos;

layout(location = 0) out vec3 v_color;

void main()
{
    gl_Position = vec4(in_pos, 0.0, 1.0);
    v_color     = vec3(1.0, 0.0, 0.0);
}
