/*
 * canvas_image.frag.slang — Canvas image fragment shader in Slang.
 * Bindless textures + sampler.
 */

#include "flux_sdf.slang"
#include "canvas_image_common.slang"

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

[shader("fragment")]
float4 main(FragmentInput input) : SV_Target {
    float2 uv = pc.image_src.xy + input.v_uv * pc.image_src.zw;
    uint32_t kind = pc.kind & 0xFFu;
    uint32_t ih = pc.image_handle & 0x0FFFFFFFu;
    uint32_t sh = pc.sampler_handle & 0x0FFFFFFFu;

    float4 texel =
        u_textures[NonUniformResourceIndex(ih)].Sample(u_samplers[NonUniformResourceIndex(sh)], uv);

    if (kind == 6u || kind == 7u)
        texel.a = 1.0f;

    texel = flux_canvas_decode_texel(texel, pc.kind, sh, pc.color_params);

    if (kind == 4u) {
        return input.v_color * texel.r;
    } else {
        float4 out_col = texel * input.v_color;
        if (kind == 6u || kind == 7u)
            out_col.a = 1.0f;
        if (kind == 5u || kind == 7u) {
            float dist =
                flux_sdf_round_rect(input.v_pos - pc.image_dst.xy, pc.image_dst.zw, pc.grad_radius);
            float coverage = clamp(0.5f - dist / max(fwidth(dist), 1e-4f), 0.0f, 1.0f);
            out_col *= coverage;
        }
        return out_col;
    }
}
