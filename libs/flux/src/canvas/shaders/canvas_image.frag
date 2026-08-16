#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_buffer_reference     : enable
#extension GL_GOOGLE_include_directive : require

/*
 * Canvas image fragment shader. Bindless textures + sampler.
 *
 * UV comes from the image quad's vertices. This keeps texture mapping
 * affine-correct when the canvas rotates, scales, or skews the quad.
 *
 * ADR-0069: sampled texels must arrive in the linear working space.
 * 8-bit UNORM images hold sRGB-encoded content and are decoded here
 * (FLUX_CANVAS_PUSH_DECODE_SRGB in pc.kind); *_SRGB images are decoded
 * by the sampler hardware and 16F images are already linear — both
 * arrive raw. Coverage (kind 4) is not a colour and stays untouched.
 *
 * Bindless layout:
 *   set 0, binding 0 — SAMPLED_IMAGE[]   (FLUX_BINDLESS_BIND_SAMPLED_IMAGE)
 *   set 0, binding 2 — SAMPLER[]         (FLUX_BINDLESS_BIND_SAMPLER)
 */

#include "canvas_color.glsl"

layout(set = 0, binding = 0) uniform texture2D u_textures[];
layout(set = 0, binding = 2) uniform sampler   u_samplers[];

struct VertexUnused {
    vec2 pos;
    uint color;
    uint _pad;
};
layout(buffer_reference, std430) readonly buffer VBUnused { VertexUnused v[]; };

/* ADR-0069/0070: per-image color parameters (flux_image_color_params).
 * Read only when pc.color_params != 0. */
struct ColorParams {
    vec4 primaries[3];  /* content -> working space, column-major mat3 */
    uint transfer;      /* flux_transfer_func */
    float gamma;
    uint lut_handle;    /* baked ICC 3D LUT (2D layout), 0xFFFFFFFF = none */
    uint lut_size;
};
layout(buffer_reference, std430) readonly buffer ColorParamsRef { ColorParams cp; };

struct GradStop { float t; uint color; };

layout(push_constant) uniform PC {
    VBUnused verts;
    vec2     inv_window_size;
    ColorParamsRef color_params; /* live iff kind & HAS_COLOR_PARAMS */
    uint     kind;
    uint     num_stops;
    vec2     grad_from;
    vec2     grad_to;
    float    grad_radius;
    float    _pad1;
    GradStop stops[8];
    uint     image_handle;
    uint     sampler_handle;
    vec4     image_dst;     /* reserved (shared push layout with SDF pipeline) */
    vec4     image_src;     /* u, v, du, dv — sampled sub-rect (normalised) */
} pc;

layout(location = 0) in  vec4 v_color;
layout(location = 1) in  vec2 v_pos;
layout(location = 2) in  vec2 v_uv;
layout(location = 0) out vec4 out_color;

/* Baked ICC LUT: 2D-laid-out (N² × N), R fastest then G then B; the
 * blue slice is lerped manually. */
vec3 sample_lut3d(uint ih, uint sh, float n, vec3 e)
{
    vec3 c = clamp(e, 0.0, 1.0) * (n - 1.0);
    float b0 = floor(c.b);
    float b1 = min(b0 + 1.0, n - 1.0);
    vec2 uv0 = vec2((b0 * n + c.r + 0.5) / (n * n), (c.g + 0.5) / n);
    vec2 uv1 = vec2((b1 * n + c.r + 0.5) / (n * n), (c.g + 0.5) / n);
    vec3 s0 = texture(sampler2D(u_textures[nonuniformEXT(ih)],
                                u_samplers[nonuniformEXT(sh)]), uv0).rgb;
    vec3 s1 = texture(sampler2D(u_textures[nonuniformEXT(ih)],
                                u_samplers[nonuniformEXT(sh)]), uv1).rgb;
    return mix(s0, s1, c.b - b0);
}

void main()
{
    /* Remap the quad-local [0,1] UV into the requested source sub-rect. */
    vec2 uv = pc.image_src.xy + v_uv * pc.image_src.zw;
    uint kind = pc.kind & 0xFFu;
    uint ih = pc.image_handle  & 0x0FFFFFFFu;
    uint sh = pc.sampler_handle & 0x0FFFFFFFu;
    vec4 texel = texture(
        sampler2D(
            u_textures[nonuniformEXT(ih)],
            u_samplers[nonuniformEXT(sh)]
        ), uv);
    /* Alpha-free RGB import (kind 6): the X channel is semantically
     * undefined and premultiplied decode would zero a=0 texels, so pin
     * alpha before any decode — the content is opaque straight sRGB. */
    if (kind == 6u)
        texel.a = 1.0;
    if ((pc.kind & FLUX_CANVAS_PUSH_HAS_COLOR_PARAMS) != 0u) {
        /* Tagged content (ADR-0069/0070): explicit parametric space or
         * a baked ICC 3D LUT, converting straight colour to the working
         * space and re-premultiplying. */
        ColorParams cp = pc.color_params.cp;
        float a = texel.a;
        vec3 straight = a > 0.0 ? texel.rgb / a : vec3(0.0);
        if (cp.lut_handle != 0xFFFFFFFFu) {
            straight = sample_lut3d(cp.lut_handle, sh, float(cp.lut_size), straight);
        } else {
            mat3 prim = mat3(cp.primaries[0].xyz, cp.primaries[1].xyz, cp.primaries[2].xyz);
            straight = prim * flux_tf_decode3(int(cp.transfer), cp.gamma, straight);
        }
        texel = vec4(straight * a, a);
    } else if ((pc.kind & FLUX_CANVAS_PUSH_DECODE_SRGB) != 0u) {
        texel = flux_decode_premul_srgb(texel);
    }

    if (kind == 4u) {
        /* Coverage glyph (kind 4): the texture's .r channel is alpha
         * coverage and v_color is the premultiplied tint colour carried
         * on the vertices. Multiplying yields a premultiplied tinted
         * glyph, so one colour-independent R8 coverage texture serves
         * every text colour (normal / muted / selection) — no per-colour
         * texture and no re-upload when the highlight moves. */
        out_color = v_color * texel.r;
    } else {
        /* Plain RGBA image (kind 3): v_color is a premultiplied tint.
         * Opaque white preserves the source; white with lower alpha is the
         * usual fade/cross-fade control. */
        out_color = texel * v_color;
        /* Alpha-free RGB import (kind 6): DRM XRGB/XBGR leaves the X bits
         * semantically undefined, so never feed the sampled value into the
         * output alpha channel or the blend equation. */
        if (kind == 6u)
            out_color.a = 1.0;
        if (kind == 5u) {
            vec2 q = abs(v_pos - pc.image_dst.xy) - (pc.image_dst.zw - pc.grad_radius);
            float distance = min(max(q.x, q.y), 0.0)
                           + length(max(q, vec2(0.0))) - pc.grad_radius;
            float coverage = clamp(0.5 - distance / max(fwidth(distance), 1e-4), 0.0, 1.0);
            out_color *= coverage;
        }
    }
}
