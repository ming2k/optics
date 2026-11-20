/*
 * canvas_vector.frag.slang — GPU 2D tiled analytic vector rasterizer
 * (ADR-0116 / ADR-0120): exact per-pixel area coverage using pre-binned
 * 16x16 tile segment lists and per-pixel-row backdrops, shading solid,
 * linear/radial gradients, and affine-mapped image patterns.
 */

#include "canvas_image_common.slang"
#include "canvas_tile_common.slang"

struct GradStop {
    float    t;
    uint32_t color;
};

struct PushConstants {
    uint64_t         verts;
    float2           inv_window_size;
    ColorParams*     color_params;
    uint32_t         kind; /* lower byte: 0 solid, 1 linear, 2 radial, 3 image pattern */
    uint32_t         num_stops;
    float2           grad_from;
    float2           grad_to;
    float            grad_radius;
    float            _pad1;
    GradStop         stops[8];
    GpuTileDispatch* tile_dispatch;
    uint32_t         image_handle;
    uint32_t         sampler_handle;
    float2           uv_row0;
    float2           uv_row1;
    float2           uv_off;
};

[[vk::push_constant]]
PushConstants pc;

struct FragmentInput {
    [[vk::location(0)]] float4 v_color : COLOR0;
    [[vk::location(1)]] float2 v_pos   : POSITION0;
    [[vk::location(2)]] float2 v_uv    : TEXCOORD0;
};

float4 unpack_color(uint32_t c) {
    float4 packed_srgb = float4(float((c >> 16) & 0xFFu), float((c >> 8) & 0xFFu),
                                float((c >> 0) & 0xFFu), float((c >> 24) & 0xFFu)) /
                         255.0f;
    return optics_gfx_decode_premul_srgb(packed_srgb);
}

float4 eval_grad(float t, float4 v_color) {
    if (pc.num_stops == 0u)
        return v_color;
    if (t <= pc.stops[0].t)
        return unpack_color(pc.stops[0].color);
    if (t >= pc.stops[pc.num_stops - 1u].t)
        return unpack_color(pc.stops[pc.num_stops - 1u].color);

    for (uint32_t i = 1u; i < pc.num_stops; ++i) {
        if (t <= pc.stops[i].t) {
            float t0 = pc.stops[i - 1u].t;
            float t1 = pc.stops[i].t;
            float f = (t - t0) / (t1 - t0 > 0.0f ? t1 - t0 : 1.0f);
            float4 c0 = unpack_color(pc.stops[i - 1u].color);
            float4 c1 = unpack_color(pc.stops[i].color);
            return lerp(c0, c1, f);
        }
    }
    return unpack_color(pc.stops[pc.num_stops - 1u].color);
}

[shader("fragment")]
float4 main(FragmentInput input) : SV_Target {
    float2 p = input.v_pos;
    float2 pixel = floor(p);

    GpuTileDispatch* d = pc.tile_dispatch;
    float area = tile_eval_pixel_area(d, pixel);
    float coverage = optics_gfx_vector_area_coverage(area, d.fill_rule);

    if (coverage <= 0.0f)
        discard;

    uint32_t base_kind = pc.kind & 0xFFu;
    float4 src_color;
    if (base_kind == 0u) {
        src_color = input.v_color;
    } else if (base_kind == 1u) {
        float2 dv = pc.grad_to - pc.grad_from;
        float len2 = dot(dv, dv);
        float t = clamp(dot(p - pc.grad_from, dv) / (len2 > 0.0f ? len2 : 1.0f), 0.0f, 1.0f);
        src_color = eval_grad(t, input.v_color);
    } else if (base_kind == 2u) {
        float t = clamp(length(p - pc.grad_from) / pc.grad_radius, 0.0f, 1.0f);
        src_color = eval_grad(t, input.v_color);
    } else {
        uint32_t ih = pc.image_handle & 0x0FFFFFFFu;
        uint32_t sh = pc.sampler_handle & 0x0FFFFFFFu;
        float2 uv = float2(dot(pc.uv_row0, p) + pc.uv_off.x, dot(pc.uv_row1, p) + pc.uv_off.y);
        float2 uv_ddx = float2(pc.uv_row0.x, pc.uv_row1.x);
        float2 uv_ddy = float2(pc.uv_row0.y, pc.uv_row1.y);
        float4 texel = u_textures[NonUniformResourceIndex(ih)].SampleGrad(
            u_samplers[NonUniformResourceIndex(sh)], uv, uv_ddx, uv_ddy);
        if (base_kind == 6u)
            texel.a = 1.0f;
        texel = optics_gfx_canvas_decode_texel(texel, pc.kind, sh, pc.color_params);
        src_color = texel * input.v_color;
        if (base_kind == 6u)
            src_color.a = 1.0f;
    }

    return float4(src_color.rgb * coverage, src_color.a * coverage);
}
