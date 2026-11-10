/*
 * canvas_solid.vert.slang — Canvas vertex shader in Slang.
 *
 * Vertex pulling via buffer device address: push constants carry the
 * GPU pointer to the per-frame vertex slice, and the shader indexes
 * it by SV_VertexID. No vertex input layout configured.
 */

#include "canvas_color.slang"

struct Vertex {
    float2   pos;
    uint32_t color;
    uint32_t _pad;
};

struct PushConstants {
    Vertex*  verts;
    float2   inv_window_size;
    float2   _pad0;
};

[[vk::push_constant]]
PushConstants pc;

struct VertexOutput {
    float4 sv_position : SV_Position;
    [[vk::location(0)]] float4 v_color : COLOR0;
    [[vk::location(1)]] float2 v_pos   : POSITION0;
    [[vk::location(2)]] float2 v_uv    : TEXCOORD0;
};

[shader("vertex")]
VertexOutput main(uint vertexIndex : SV_VertexID)
{
    VertexOutput output;
    Vertex v = pc.verts[vertexIndex];
    float2 ndc = v.pos * pc.inv_window_size - float2(1.0f, 1.0f);
    output.sv_position = float4(ndc, 0.0f, 1.0f);

    uint32_t c = v.color;
    float4 packed_srgb = float4(
        float((c >> 16) & 0xFFu),
        float((c >>  8) & 0xFFu),
        float((c >>  0) & 0xFFu),
        float((c >> 24) & 0xFFu)
    ) / 255.0f;
    output.v_color = flux_decode_premul_srgb(packed_srgb);
    output.v_pos = v.pos;
    output.v_uv = float2(float(v._pad & 0xFFFFu), float(v._pad >> 16)) / 65535.0f;
    return output;
}
