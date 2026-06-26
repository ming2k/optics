#version 450
#extension GL_EXT_nonuniform_qualifier : require

/*
 * Batched glyph-run fragment shader (ADR-0010).
 *
 * Each vertex carries its own atlas UV (unpacked from the vertex
 * `_pad` field by canvas_solid.vert), so one draw covers an entire
 * pre-shaped run instead of a draw per glyph. The atlas's R channel
 * is alpha coverage; v_color is the premultiplied per-glyph tint —
 * the product is a premultiplied tinted glyph, same contract as the
 * per-glyph coverage path (kind 4 in canvas_image.frag).
 *
 * Bindless layout:
 *   set 0, binding 0 — SAMPLED_IMAGE[]   (FLUX_BINDLESS_BIND_SAMPLED_IMAGE)
 *   set 0, binding 2 — SAMPLER[]         (FLUX_BINDLESS_BIND_SAMPLER)
 */

layout(set = 0, binding = 0) uniform texture2D u_textures[];
layout(set = 0, binding = 2) uniform sampler   u_samplers[];

layout(push_constant) uniform PC {
    uvec2    verts_address;
    vec2     inv_window_size;
    vec2     _pad0;
    uint     kind;
    uint     num_stops;
    vec2     grad_from;
    vec2     grad_to;
    float    grad_radius;
    float    _pad1;
    uvec2    stops[8];
    uint     image_handle;
    uint     sampler_handle;
    vec4     image_dst;
    vec4     image_src;
} pc;

layout(location = 0) in  vec4 v_color;
layout(location = 1) in  vec2 v_pos;
layout(location = 2) in  vec2 v_uv;
layout(location = 0) out vec4 out_color;

void main()
{
    uint ih = pc.image_handle   & 0x0FFFFFFFu;
    uint sh = pc.sampler_handle & 0x0FFFFFFFu;
    float coverage = texture(
        sampler2D(
            u_textures[nonuniformEXT(ih)],
            u_samplers[nonuniformEXT(sh)]
        ), v_uv).r;
    out_color = v_color * coverage;
}
