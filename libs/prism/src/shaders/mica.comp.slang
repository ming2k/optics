// mica.comp.slang
// Mica material shader (Windows 11 Fluent Design inspired) (ADR-0063, ADR-0156).

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

struct MicaPush {
    uint wallpaper_handle;
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
    float tint_opacity;
    float luminosity_plate;
    float luminosity_opacity;
    float noise_intensity;
    float border_width;
    float border_alpha;
    float shadow_alpha;
    float shadow_blur;
    float shadow_offset_y;
    uint tint_color;
    uint group_width;
    uint group_height;
    float screen_origin_x;
    float screen_origin_y;
    float screen_width;
    float screen_height;
    uint fallback_color;
    float fallback_weight;
    uint is_alt;
    float slab_origin_x;
    float slab_origin_y;
    float capture_width;
    float capture_height;
};
[[vk::push_constant]] MicaPush pc;

float3 unpack_rgb(uint c)
{
    return float3(
        float((c >> 16u) & 0xFFu) / 255.0,
        float((c >> 8u) & 0xFFu) / 255.0,
        float(c & 0xFFu) / 255.0
    );
}

float prng_dither(float2 uv)
{
    float n = frac(sin(dot(uv, float2(12.9898, 78.233))) * 43758.5453);
    return (n - 0.5);
}

float4 sample_image(uint handle, float2 uv)
{
    return u_sampled[NonUniformResourceIndex(handle & 0x0FFFFFFFu)].SampleLevel(
        u_samplers[NonUniformResourceIndex(pc.sampler_handle & 0x0FFFFFFFu)],
        clamp(uv, float2(0.0), float2(1.0)),
        0.0
    );
}

void store_source_over(uint2 pixel, float4 source)
{
    uint handle = NonUniformResourceIndex(pc.output_handle & 0x0FFFFFFFu);
    float4 destination = u_storage[handle][pixel];
    u_storage[handle][pixel] = source + destination * (1.0 - source.a);
}

float rounded_rect_sdf(float2 p, float4 bounds, float requested_radius)
{
    float2 half_extent = max(bounds.zw * 0.5, float2(0.001));
    float2 centre = bounds.xy + half_extent;
    return flux_sdf_round_rect(p - centre, half_extent, requested_radius);
}

[numthreads(16, 16, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 local = dispatchThreadID.xy;
    if (local.x >= pc.group_width || local.y >= pc.group_height) return;

    uint2 pixel = local + uint2(pc.origin_x, pc.origin_y);
    if (pixel.x >= pc.width || pixel.y >= pc.height) return;

    float2 p = float2(pixel) + float2(0.5) + float2(pc.slab_origin_x, pc.slab_origin_y);

    // Screen-anchored or local wallpaper UV
    float2 wallpaper_uv;
    if (pc.screen_width > 0.0 && pc.screen_height > 0.0) {
        float2 screen_coord = p + float2(pc.screen_origin_x, pc.screen_origin_y);
        wallpaper_uv = screen_coord / float2(pc.screen_width, pc.screen_height);
    } else {
        wallpaper_uv = p / max(float2(pc.capture_width, pc.capture_height), float2(1.0));
    }

    float d = rounded_rect_sdf(p, pc.bounds, pc.corner_radius);

    // Drop shadow computation: solid inside shadow footprint (sd <= 0), Gaussian falloff outside (sd > 0)
    float shadow_cov = 0.0;
    if (pc.shadow_alpha > 0.001 && pc.shadow_blur > 0.0) {
        float2 sp = p - float2(0.0, pc.shadow_offset_y);
        float sd = rounded_rect_sdf(sp, pc.bounds, pc.corner_radius);
        float sigma = max(pc.shadow_blur * 0.5, 0.5);
        shadow_cov = pc.shadow_alpha * (sd <= 0.0 ? 1.0 : exp(-0.5 * (sd * sd) / (sigma * sigma)));
    }

    if (d > 1.5 && shadow_cov <= 0.001) return;

    // Body coverage
    float body_cov = clamp(0.5 - d, 0.0, 1.0) * pc.opacity;

    // Sample wallpaper (prefer blurred wallpaper, fall back to sharp)
    uint sample_handle = (pc.blurred_handle != 0u && pc.blurred_handle != 0xFFFFFFFFu)
                             ? pc.blurred_handle
                             : pc.wallpaper_handle;
    float4 sampled_wp = sample_image(sample_handle, wallpaper_uv);

    // Luminosity plate balancing
    // Plate polarity: 0 = smoke (dark mode), 1 = pearl (light mode)
    float3 smoke_col = (pc.is_alt != 0u) ? float3(0.08) : float3(0.12);
    float3 pearl_col = (pc.is_alt != 0u) ? float3(0.92) : float3(0.95);
    float3 plate_color = lerp(smoke_col, pearl_col, clamp(pc.luminosity_plate, 0.0, 1.0));

    float lum_weight = clamp(pc.luminosity_opacity, 0.0, 1.0);
    float3 plate_blended = lerp(sampled_wp.rgb, plate_color, lum_weight);

    // Tint layer
    float3 tint = unpack_rgb(pc.tint_color);
    float tint_weight = clamp(pc.tint_opacity, 0.0, 1.0);
    if (pc.is_alt != 0u) {
        tint_weight = min(tint_weight * 1.35, 1.0);
    }
    float3 tinted = lerp(plate_blended, tint, tint_weight);

    // Inactive fallback blend
    if (pc.fallback_weight > 0.001) {
        float3 fallback_rgb = unpack_rgb(pc.fallback_color);
        tinted = lerp(tinted, fallback_rgb, clamp(pc.fallback_weight, 0.0, 1.0));
    }

    // 1px subtle SDF border highlight rim / contact crease
    if (pc.border_alpha > 0.001 && pc.border_width > 0.0) {
        float edge_dist = abs(d);
        float border_intensity = clamp(1.0 - edge_dist / max(pc.border_width, 0.5), 0.0, 1.0);
        float3 rim_col = lerp(float3(1.0), float3(0.12, 0.15, 0.20), clamp(pc.luminosity_plate, 0.0, 1.0));
        tinted = lerp(tinted, rim_col, border_intensity * pc.border_alpha);
    }

    // Procedural noise dither
    float grain = prng_dither(float2(pixel)) * pc.noise_intensity;
    float3 body_rgb = clamp(tinted + float3(grain), 0.0, 1.0);

    // Combine body and drop shadow
    float4 body_out = float4(body_rgb * body_cov, body_cov);
    float4 shadow_out = float4(float3(0.0), shadow_cov * (1.0 - body_cov));

    float4 final_color = body_out + shadow_out;
    if (final_color.a > 0.001) {
        store_source_over(pixel, final_color);
    }
}
