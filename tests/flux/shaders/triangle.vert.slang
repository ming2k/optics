/*
 * triangle.vert.slang — Hard-coded NDC triangle in Slang.
 * Used by test_graphics_pipeline to verify pipeline creation end-to-end.
 */

static const float2 positions[3] = {
    float2( 0.0f, -0.5f),
    float2( 0.5f,  0.5f),
    float2(-0.5f,  0.5f)
};

static const float3 colors[3] = {
    float3(1.0f, 0.0f, 0.0f),
    float3(0.0f, 1.0f, 0.0f),
    float3(0.0f, 0.0f, 1.0f)
};

struct VertexOutput {
    float4 sv_position : SV_Position;
    [[vk::location(0)]] float3 v_color : COLOR0;
};

[shader("vertex")]
VertexOutput main(uint vertexIndex : SV_VertexID) {
    VertexOutput output;
    output.sv_position = float4(positions[vertexIndex], 0.0f, 1.0f);
    output.v_color = colors[vertexIndex];
    return output;
}
