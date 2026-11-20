/*
 * canvas_image_common.slang — shared bindless texture/sampler bindings and
 * colour-management decode helpers for canvas image and vector fragment shaders.
 */

#ifndef CANVAS_IMAGE_COMMON_SLANG
#define CANVAS_IMAGE_COMMON_SLANG

#include "canvas_color.slang"

[[vk::binding(0, 0)]] Texture2D u_textures[];
[[vk::binding(2, 0)]] SamplerState u_samplers[];

struct ColorParams {
    float4   primaries[3];
    uint32_t transfer;
    float    gamma;
    uint32_t lut_handle;
    uint32_t lut_size;
};

float3 optics_gfx_canvas_sample_lut3d(uint32_t ih, uint32_t sh, float n, float3 e) {
    float3 c = clamp(e, 0.0f, 1.0f) * (n - 1.0f);
    float b0 = floor(c.b);
    float b1 = min(b0 + 1.0f, n - 1.0f);
    float2 uv0 = float2((b0 * n + c.r + 0.5f) / (n * n), (c.g + 0.5f) / n);
    float2 uv1 = float2((b1 * n + c.r + 0.5f) / (n * n), (c.g + 0.5f) / n);
    float3 s0 = u_textures[NonUniformResourceIndex(ih)]
                    .SampleLevel(u_samplers[NonUniformResourceIndex(sh)], uv0, 0.0f)
                    .rgb;
    float3 s1 = u_textures[NonUniformResourceIndex(ih)]
                    .SampleLevel(u_samplers[NonUniformResourceIndex(sh)], uv1, 0.0f)
                    .rgb;
    return lerp(s0, s1, c.b - b0);
}

float4 optics_gfx_canvas_decode_texel(float4 texel, uint32_t kind, uint32_t sh, ColorParams* color_params) {
    if ((kind & OPTICS_GFX_CANVAS_PUSH_HAS_COLOR_PARAMS) != 0u) {
        ColorParams cp = *color_params;
        float a = texel.a;
        float3 straight = a > 0.0f ? texel.rgb / a : float3(0.0f, 0.0f, 0.0f);
        if (cp.lut_handle != 0xFFFFFFFFu) {
            straight = optics_gfx_canvas_sample_lut3d(cp.lut_handle, sh, float(cp.lut_size), straight);
        } else {
            float3 dec = optics_gfx_decode_to_working(int(cp.transfer), cp.gamma, straight);
            straight = cp.primaries[0].xyz * dec.x + cp.primaries[1].xyz * dec.y +
                       cp.primaries[2].xyz * dec.z;
        }
        return float4(straight * a, a);
    }
    if ((kind & OPTICS_GFX_CANVAS_PUSH_DECODE_SRGB) != 0u)
        return optics_gfx_decode_premul_srgb(texel);
    return texel;
}

#endif /* CANVAS_IMAGE_COMMON_SLANG */
