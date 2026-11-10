/*
 * canvas_image.frag.slang — Canvas image fragment shader in Slang.
 * Bindless textures + sampler.
 */

#include "flux_sdf.slang"
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

struct GradStop {
    float    t;
    uint32_t color;
};

struct PushConstants {
    uint64_t     verts;
    float2       inv_window_size;
    ColorParams* color_params;
    uint32_t     kind;
    uint32_t     num_stops;
    float2       grad_from;
    float2       grad_to;
    float        grad_radius;
    float        _pad1;
    GradStop     stops[8];
    uint32_t     image_handle;
    uint32_t     sampler_handle;
    float4       image_dst;
    float4       image_src;
};

[[vk::push_constant]]
PushConstants pc;

struct FragmentInput {
    [[vk::location(0)]] float4 v_color : COLOR0;
    [[vk::location(1)]] float2 v_pos   : POSITION0;
    [[vk::location(2)]] float2 v_uv    : TEXCOORD0;
};

float3 sample_lut3d(uint32_t ih, uint32_t sh, float n, float3 e)
{
    float3 c = clamp(e, 0.0f, 1.0f) * (n - 1.0f);
    float b0 = floor(c.b);
    float b1 = min(b0 + 1.0f, n - 1.0f);
    float2 uv0 = float2((b0 * n + c.r + 0.5f) / (n * n), (c.g + 0.5f) / n);
    float2 uv1 = float2((b1 * n + c.r + 0.5f) / (n * n), (c.g + 0.5f) / n);
    float3 s0 = u_textures[NonUniformResourceIndex(ih)].Sample(u_samplers[NonUniformResourceIndex(sh)], uv0).rgb;
    float3 s1 = u_textures[NonUniformResourceIndex(ih)].Sample(u_samplers[NonUniformResourceIndex(sh)], uv1).rgb;
    return lerp(s0, s1, c.b - b0);
}

[shader("fragment")]
float4 main(FragmentInput input) : SV_Target
{
    float2 uv = pc.image_src.xy + input.v_uv * pc.image_src.zw;
    uint32_t kind = pc.kind & 0xFFu;
    uint32_t ih = pc.image_handle   & 0x0FFFFFFFu;
    uint32_t sh = pc.sampler_handle & 0x0FFFFFFFu;

    float4 texel = u_textures[NonUniformResourceIndex(ih)].Sample(u_samplers[NonUniformResourceIndex(sh)], uv);

    if (kind == 6u || kind == 7u)
        texel.a = 1.0f;

    if ((pc.kind & FLUX_CANVAS_PUSH_HAS_COLOR_PARAMS) != 0u) {
        ColorParams cp = *pc.color_params;
        float a = texel.a;
        float3 straight = a > 0.0f ? texel.rgb / a : float3(0.0f, 0.0f, 0.0f);
        if (cp.lut_handle != 0xFFFFFFFFu) {
            straight = sample_lut3d(cp.lut_handle, sh, float(cp.lut_size), straight);
        } else {
            float3 dec = flux_decode_to_working(int(cp.transfer), cp.gamma, straight);
            straight = cp.primaries[0].xyz * dec.x +
                       cp.primaries[1].xyz * dec.y +
                       cp.primaries[2].xyz * dec.z;
        }
        texel = float4(straight * a, a);
    } else if ((pc.kind & FLUX_CANVAS_PUSH_DECODE_SRGB) != 0u) {
        texel = flux_decode_premul_srgb(texel);
    }

    if (kind == 4u) {
        return input.v_color * texel.r;
    } else {
        float4 out_col = texel * input.v_color;
        if (kind == 6u || kind == 7u)
            out_col.a = 1.0f;
        if (kind == 5u || kind == 7u) {
            float dist = flux_sdf_round_rect(input.v_pos - pc.image_dst.xy, pc.image_dst.zw, pc.grad_radius);
            float coverage = clamp(0.5f - dist / max(fwidth(dist), 1e-4f), 0.0f, 1.0f);
            out_col *= coverage;
        }
        return out_col;
    }
}
