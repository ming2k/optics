/*
 * effect_blur.comp.slang — Separable Gaussian blur in Slang.
 */

[[vk::binding(0, 0)]] Texture2D u_sampled[];
#ifdef EFFECT_STORAGE_RGBA16F
[[vk::binding(1, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> u_storage[];
#else
[[vk::binding(1, 0)]] [[vk::image_format("rgba8")]] RWTexture2D<float4> u_storage[];
#endif
[[vk::binding(2, 0)]] SamplerState u_samplers[];

struct PushConstants {
    uint32_t in_handle;
    uint32_t sampler_handle;
    uint32_t out_handle;
    uint32_t width;
    uint32_t height;
    int32_t  radius;
    float    sigma;
    uint32_t axis; /* 0 = horizontal, 1 = vertical */
};

[[vk::push_constant]]
PushConstants pc;

groupshared float kernel_weights[65];

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint3 groupThreadID : SV_GroupThreadID, uint3 dispatchID : SV_DispatchThreadID)
{
    uint32_t local_index = groupThreadID.y * 16u + groupThreadID.x;
    if (local_index <= uint32_t(pc.radius)) {
        float i = float(local_index);
        float two_sigma_sq = 2.0f * pc.sigma * pc.sigma;
        kernel_weights[local_index] = exp(-(i * i) / two_sigma_sq);
    }
    GroupMemoryBarrierWithGroupSync();

    uint2 p = dispatchID.xy;
    if (p.x >= pc.width || p.y >= pc.height) return;

    uint32_t ih = pc.in_handle      & 0x0FFFFFFFu;
    uint32_t sh = pc.sampler_handle & 0x0FFFFFFFu;
    uint32_t oh = pc.out_handle     & 0x0FFFFFFFu;

    float2 inv_size = 1.0f / float2(float(pc.width), float(pc.height));
    float2 base_uv  = (float2(p) + 0.5f) * inv_size;

    if (pc.radius == 0) {
        float4 c = u_sampled[NonUniformResourceIndex(ih)].Sample(u_samplers[NonUniformResourceIndex(sh)], base_uv);
        u_storage[NonUniformResourceIndex(oh)][int2(p)] = c;
        return;
    }

    float  weight_sum = 0.0f;
    float4 colour_sum = float4(0.0f, 0.0f, 0.0f, 0.0f);

    for (int32_t i = -pc.radius; i <= pc.radius; ++i) {
        float2 off = (pc.axis == 0u) ? float2(float(i), 0.0f) : float2(0.0f, float(i));
        float2 uv  = base_uv + off * inv_size;
        float  w   = kernel_weights[abs(i)];
        float4 c   = u_sampled[NonUniformResourceIndex(ih)].Sample(u_samplers[NonUniformResourceIndex(sh)], uv);
        colour_sum += c * w;
        weight_sum += w;
    }

    float4 result = colour_sum / weight_sum;
    u_storage[NonUniformResourceIndex(oh)][int2(p)] = result;
}
