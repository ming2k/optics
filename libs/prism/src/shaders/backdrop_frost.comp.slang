// backdrop_frost.comp.slang
// Base-material pass of the layered backdrop compositor (ADR-0063, ADR-0156).

#include "flux/shaders/flux_sdf.slang"

[[vk::binding(0, 0)]] Texture2D u_sampled[];
[[vk::binding(1, 0)]]
#ifdef PRISM_STORAGE_RGBA16F
[[vk::image_format("rgba16f")]]
#else
[[vk::image_format("rgba8")]]
#endif
RWTexture2D<float4> u_storage[];
[[vk::binding(2, 0)]] SamplerState u_samplers[];

static const uint BACKDROP_FROST_MODE_BASE = 0u;
static const uint BACKDROP_FROST_MODE_PLATE = 1u;

struct BackdropFrostPush {
    uint input_handle;
    uint blurred_handle;
    uint sampler_handle;
    uint output_handle;
    uint width;
    uint height;
    uint origin_x;
    uint origin_y;
    float4 bounds;
    float corner_radius;
    float opacity;
    uint tint_color;
    float tint_strength;
    uint region_width;
    uint region_height;
    uint mode;
    uint reserved;
    float slab_origin_x;
    float slab_origin_y;
    float capture_width;
    float capture_height;
};
[[vk::push_constant]] BackdropFrostPush pc;

float rounded_rect_sdf(float2 p, float4 bounds, float requested_radius)
{
    float2 half_extent = max(bounds.zw * 0.5, float2(0.001));
    float2 centre = bounds.xy + half_extent;
    return flux_sdf_round_rect(p - centre, half_extent, requested_radius);
}

float3 unpack_rgb(uint c)
{
    return float3(
        float((c >> 16u) & 0xFFu) / 255.0,
        float((c >> 8u) & 0xFFu) / 255.0,
        float(c & 0xFFu) / 255.0
    );
}

float4 sample_rgb(uint handle, float2 uv)
{
    return u_sampled[NonUniformResourceIndex(handle & 0x0FFFFFFFu)].SampleLevel(
        u_samplers[NonUniformResourceIndex(pc.sampler_handle & 0x0FFFFFFFu)],
        clamp(uv, float2(0.0), float2(1.0)),
        0.0
    );
}

[numthreads(16, 16, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 local = dispatchThreadID.xy;
    if (local.x >= pc.region_width || local.y >= pc.region_height) return;

    uint2 pixel = local + uint2(pc.origin_x, pc.origin_y);
    if (pixel.x >= pc.width || pixel.y >= pc.height) return;

    float2 p = float2(pixel) + float2(0.5) + float2(pc.slab_origin_x, pc.slab_origin_y);
    float2 uv = p / max(float2(pc.capture_width, pc.capture_height), float2(1.0));

    uint handle = NonUniformResourceIndex(pc.output_handle & 0x0FFFFFFFu);

    if (pc.mode == BACKDROP_FROST_MODE_BASE) {
        u_storage[handle][pixel] = float4(sample_rgb(pc.input_handle, uv).rgb, 1.0);
        return;
    }

    float coverage = clamp(0.5 - rounded_rect_sdf(p, pc.bounds, pc.corner_radius), 0.0, 1.0) *
                     clamp(pc.opacity, 0.0, 1.0);
    if (coverage <= 0.0) return;

    float3 body = lerp(sample_rgb(pc.blurred_handle, uv).rgb, unpack_rgb(pc.tint_color),
                       clamp(pc.tint_strength, 0.0, 1.0));

    float4 destination = u_storage[handle][pixel];
    float4 source = float4(body * coverage, coverage);
    u_storage[handle][pixel] = source + destination * (1.0 - coverage);
}
