/*
 * triangle_attr.vert.slang — Vertex-buffer variant of triangle in Slang.
 */

struct VertexInput {
    [[vk::location(0)]] float2 in_pos : POSITION0;
};

struct VertexOutput {
    float4 sv_position : SV_Position;
    [[vk::location(0)]] float3 v_color : COLOR0;
};

[shader("vertex")]
VertexOutput main(VertexInput input) {
    VertexOutput output;
    output.sv_position = float4(input.in_pos, 0.0f, 1.0f);
    output.v_color = float3(1.0f, 0.0f, 0.0f);
    return output;
}
