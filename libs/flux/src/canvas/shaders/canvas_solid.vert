#version 450
#extension GL_GOOGLE_include_directive : require

/*
 * Canvas vertex shader. Shared by solid + gradient frag shaders.
 *
 * Vertex pulling via buffer device address: push constants carry the
 * GPU pointer to the per-frame vertex slice, and the shader indexes
 * it by gl_VertexIndex. No vertex input layout configured.
 *
 * Vertex colours are packed premultiplied sRGB (flux_color); they are
 * decoded to premultiplied linear here (ADR-0069) so every downstream
 * fragment shader and the fixed-function blender work in the linear
 * working space.
 *
 * Outputs:
 *   v_color  - interpolated vertex colour (used by solid frag)
 *   v_pos    - pixel-space position       (used by gradient frag)
 *
 * Push-constant struct matches flux_canvas_push in src/canvas/internal.h.
 */

#extension GL_EXT_buffer_reference : enable

#include "canvas_color.glsl"

struct Vertex {
    vec2 pos;
    uint color;
    uint _pad;
};

layout(buffer_reference, std430) readonly buffer VertexBuffer {
    Vertex verts[];
};

layout(push_constant) uniform PC {
    VertexBuffer verts;
    vec2         inv_window_size;
    vec2         _pad0;
    /* gradient fields follow; vertex shader doesn't read them */
} pc;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_pos;
layout(location = 2) out vec2 v_uv;

void main()
{
    Vertex v   = pc.verts.verts[gl_VertexIndex];
    vec2   ndc = v.pos * pc.inv_window_size - vec2(1.0);
    gl_Position = vec4(ndc, 0.0, 1.0);

    uint c = v.color;
    vec4 packed_srgb = vec4(
        float((c >> 16) & 0xFFu),
        float((c >>  8) & 0xFFu),
        float((c >>  0) & 0xFFu),
        float((c >> 24) & 0xFFu)
    ) / 255.0;
    v_color = flux_decode_premul_srgb(packed_srgb);

    v_pos = v.pos;

    /* Per-vertex UV packed as unorm16x2 in `_pad`. Image and glyph
     * fragment shaders consume it; other pipelines ignore the varying. */
    v_uv = vec2(float(v._pad & 0xFFFFu),
                float(v._pad >> 16)) / 65535.0;
}
