/*
 * effect_backdrop.comp.slang — Fixed-cost Dual-Kawase filter for animated backdrops in Slang.
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
    uint32_t input_width;
    uint32_t input_height;
    uint32_t output_width;
    uint32_t output_height;
    float    offset;
    uint32_t mode; /* 0 = downsample, 1 = upsample, 2 = copy */
    uint32_t origin_x;
    uint32_t origin_y;
    uint32_t region_width;
    uint32_t region_height;
};

[[vk::push_constant]]
PushConstants pc;

float4 sample_input(uint32_t image_handle, uint32_t sampler_handle, float2 uv, float2 uv_min, float2 uv_max)
{
    return u_sampled[NonUniformResourceIndex(image_handle)].Sample(
        u_samplers[NonUniformResourceIndex(sampler_handle)],
        clamp(uv, uv_min, uv_max)
    );
}

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint3 dispatchID : SV_DispatchThreadID)
{
    uint2 local = dispatchID.xy;
    if (local.x >= pc.region_width || local.y >= pc.region_height) return;
    uint2 p = local + uint2(pc.origin_x, pc.origin_y);
    if (p.x >= pc.output_width || p.y >= pc.output_height) return;

    uint32_t ih = pc.in_handle      & 0x0FFFFFFFu;
    uint32_t sh = pc.sampler_handle & 0x0FFFFFFFu;
    uint32_t oh = pc.out_handle     & 0x0FFFFFFFu;

    float2 inv_out = 1.0f / float2(float(pc.output_width), float(pc.output_height));
    float2 uv_min = (float2(float(pc.origin_x), float(pc.origin_y)) + 0.5f) * inv_out;
    float2 uv_max = max(
        uv_min,
        (float2(float(pc.origin_x + pc.region_width), float(pc.origin_y + pc.region_height)) - 0.5f) * inv_out
    );
    float2 uv = (float2(p) + 0.5f) * inv_out;

    if (pc.mode == 2u) {
        u_storage[NonUniformResourceIndex(oh)][int2(p)] = sample_input(ih, sh, uv, uv_min, uv_max);
        return;
    }

    float2 half_pixel = (0.5f * pc.offset) / float2(float(pc.input_width), float(pc.input_height));
    float4 colour;
    if (pc.mode == 0u) {
        colour = sample_input(ih, sh, uv, uv_min, uv_max) * 4.0f;
        colour += sample_input(ih, sh, uv + half_pixel * float2(-1.0f, -1.0f), uv_min, uv_max);
        colour += sample_input(ih, sh, uv + half_pixel * float2( 1.0f, -1.0f), uv_min, uv_max);
        colour += sample_input(ih, sh, uv + half_pixel * float2(-1.0f,  1.0f), uv_min, uv_max);
        colour += sample_input(ih, sh, uv + half_pixel * float2( 1.0f,  1.0f), uv_min, uv_max);
        colour *= 0.125f;
    } else {
        colour  = sample_input(ih, sh, uv + half_pixel * float2(-2.0f,  0.0f), uv_min, uv_max);
        colour += sample_input(ih, sh, uv + half_pixel * float2(-1.0f,  1.0f), uv_min, uv_max) * 2.0f;
        colour += sample_input(ih, sh, uv + half_pixel * float2( 0.0f,  2.0f), uv_min, uv_max);
        colour += sample_input(ih, sh, uv + half_pixel * float2( 1.0f,  1.0f), uv_min, uv_max) * 2.0f;
        colour += sample_input(ih, sh, uv + half_pixel * float2( 2.0f,  0.0f), uv_min, uv_max);
        colour += sample_input(ih, sh, uv + half_pixel * float2( 1.0f, -1.0f), uv_min, uv_max) * 2.0f;
        colour += sample_input(ih, sh, uv + half_pixel * float2( 0.0f, -2.0f), uv_min, uv_max);
        colour += sample_input(ih, sh, uv + half_pixel * float2(-1.0f, -1.0f), uv_min, uv_max) * 2.0f;
        colour /= 12.0f;
    }

    u_storage[NonUniformResourceIndex(oh)][int2(p)] = colour;
}
