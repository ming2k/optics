/*
 * scene_color.slang — Scene color management in Slang (ADR-0069).
 */

#ifndef SCENE_COLOR_SLANG
#define SCENE_COLOR_SLANG

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

struct UnlitParams {
    float4       base_color;
    float4       uv_scale_offset;
    float4       uv_rotation_alpha_cutoff;
    uint4        texture_info;
    ColorParams* color_params;
    uint32_t     color_flags;
    uint32_t     _color_pad;
    float4x4*    palette;
    uint32_t     joint_count;
    uint32_t     _pad;
};

struct PhongParams {
    float4x4     world;
    float4       nrm0;
    float4       nrm1;
    float4       nrm2;
    float4       base_color;
    float4       uv_scale_offset;
    float4       uv_rotation_alpha_cutoff;
    uint4        texture_info;
    ColorParams* color_params;
    uint32_t     color_flags;
    uint32_t     _color_pad;
    float4       light_dir_shininess;
    float4       light_color_ambient;
    float4       eye_specular;
    float4x4*    palette;
    uint32_t     joint_count;
    uint32_t     _pad;
};

struct GpuCurveSeg {
    float x0, y0;
    float x1, y1;
};

struct VectorParams {
    float4       base_color;
    float4       uv_scale_offset;
    float4       uv_rotation_alpha_cutoff;
    uint4        texture_info;
    ColorParams* color_params;
    uint32_t     color_flags;
    uint32_t     _color_pad;
    GpuCurveSeg* segs;
    uint32_t     seg_count;
    uint32_t     fill_rule;
    uint32_t     _pad[2];
};

static const uint32_t SCENE_COLOR_DECODE     = 0x1u;
static const uint32_t SCENE_COLOR_HAS_PARAMS = 0x2u;

float3 sample_lut3d(uint32_t ih, uint32_t sh, float n, float3 e) {
    float3 c = clamp(e, 0.0f, 1.0f) * (n - 1.0f);
    float b0 = floor(c.b);
    float b1 = min(b0 + 1.0f, n - 1.0f);
    float2 uv0 = float2((b0 * n + c.r + 0.5f) / (n * n), (c.g + 0.5f) / n);
    float2 uv1 = float2((b1 * n + c.r + 0.5f) / (n * n), (c.g + 0.5f) / n);
    float3 s0 = u_textures[NonUniformResourceIndex(ih)].Sample(u_samplers[NonUniformResourceIndex(sh)], uv0).rgb;
    float3 s1 = u_textures[NonUniformResourceIndex(ih)].Sample(u_samplers[NonUniformResourceIndex(sh)], uv1).rgb;
    return lerp(s0, s1, c.b - b0);
}

float3 scene_decode_tagged(ColorParams cp, uint32_t sh, float3 rgb) {
    if (cp.lut_handle != 0xFFFFFFFFu)
        return sample_lut3d(cp.lut_handle, sh, float(cp.lut_size), rgb);
    float3 dec = flux_decode_to_working(int(cp.transfer), cp.gamma, rgb);
    return cp.primaries[0].xyz * dec.x +
           cp.primaries[1].xyz * dec.y +
           cp.primaries[2].xyz * dec.z;
}

#endif /* SCENE_COLOR_SLANG */
