/*
 * canvas_glyph.frag.slang — Batched glyph-run fragment shader (ADR-0010) in Slang.
 */

[[vk::binding(0, 0)]] Texture2D u_textures[];
[[vk::binding(2, 0)]] SamplerState u_samplers[];

struct PushConstants {
    uint64_t verts_address;
    float2   inv_window_size;
    float2   _pad0;
    uint32_t kind;
    uint32_t num_stops;
    float2   grad_from;
    float2   grad_to;
    float    grad_radius;
    float    _pad1;
    uint64_t stops[8];
    uint32_t image_handle;
    uint32_t sampler_handle;
    float4   image_dst;
    float4   image_src;
};

[[vk::push_constant]]
PushConstants pc;

struct FragmentInput {
    [[vk::location(0)]] float4 v_color : COLOR0;
    [[vk::location(1)]] float2 v_pos   : POSITION0;
    [[vk::location(2)]] float2 v_uv    : TEXCOORD0;
};

[shader("fragment")]
float4 main(FragmentInput input) : SV_Target
{
    uint32_t ih = pc.image_handle   & 0x0FFFFFFFu;
    uint32_t sh = pc.sampler_handle & 0x0FFFFFFFu;
    float coverage = u_textures[NonUniformResourceIndex(ih)].Sample(u_samplers[NonUniformResourceIndex(sh)], input.v_uv).r;
    return input.v_color * coverage;
}
