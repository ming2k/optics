/*
 * canvas_output.vert.slang — Canvas output-transform vertex shader in Slang (ADR-0069).
 */

struct VertexOutput {
    float4 sv_position : SV_Position;
    [[vk::location(0)]] float2 v_uv : TEXCOORD0;
};

[shader("vertex")]
VertexOutput main(uint vertexIndex : SV_VertexID)
{
    VertexOutput output;
    float2 pos = float2(float((vertexIndex << 1) & 2), float(vertexIndex & 2));
    output.sv_position = float4(pos * 2.0f - 1.0f, 0.0f, 1.0f);
    output.v_uv = pos;
    return output;
}
