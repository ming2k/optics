#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_buffer_reference     : enable

/*
 * Canvas image fragment shader. Bindless textures + sampler.
 *
 * UV derived from v_pos and the destination rect carried in push
 * constants: uv = (v_pos - image_dst.xy) / image_dst.zw.
 *
 * Bindless layout:
 *   set 0, binding 0 — SAMPLED_IMAGE[]   (FLUX_BINDLESS_BIND_SAMPLED_IMAGE)
 *   set 0, binding 2 — SAMPLER[]         (FLUX_BINDLESS_BIND_SAMPLER)
 */

layout(set = 0, binding = 0) uniform texture2D u_textures[];
layout(set = 0, binding = 2) uniform sampler   u_samplers[];

struct VertexUnused {
    vec2 pos;
    uint color;
    uint _pad;
};
layout(buffer_reference, std430) readonly buffer VBUnused { VertexUnused v[]; };

struct GradStop { float t; uint color; };

layout(push_constant) uniform PC {
    VBUnused verts;
    vec2     inv_window_size;
    vec2     _pad0;
    uint     kind;
    uint     num_stops;
    vec2     grad_from;
    vec2     grad_to;
    float    grad_radius;
    float    _pad1;
    GradStop stops[8];
    uint     image_handle;
    uint     sampler_handle;
    vec4     image_dst;     /* x, y, w, h — pre-transform pixel rect */
    vec4     image_src;     /* u, v, du, dv — sampled sub-rect (normalised) */
} pc;

layout(location = 0) in  vec4 v_color;
layout(location = 1) in  vec2 v_pos;
layout(location = 0) out vec4 out_color;

void main()
{
    /* Local [0,1] coverage of the destination rect, then remapped into the
     * sampled sub-rect (image_src). Whole-image draws pass {0,0,1,1}, so this
     * reduces to plain UV; a glyph atlas passes the glyph's sub-rect. */
    vec2 local = (v_pos - pc.image_dst.xy) / pc.image_dst.zw;
    local = clamp(local, vec2(0.0), vec2(1.0));
    vec2 uv = pc.image_src.xy + local * pc.image_src.zw;
    uint ih = pc.image_handle  & 0x0FFFFFFFu;
    uint sh = pc.sampler_handle & 0x0FFFFFFFu;
    vec4 texel = texture(
        sampler2D(
            u_textures[nonuniformEXT(ih)],
            u_samplers[nonuniformEXT(sh)]
        ), uv);

    if (pc.kind == 4u) {
        /* Coverage glyph (kind 4): the texture's .r channel is alpha
         * coverage and v_color is the premultiplied tint colour carried
         * on the vertices. Multiplying yields a premultiplied tinted
         * glyph, so one colour-independent R8 coverage texture serves
         * every text colour (normal / muted / selection) — no per-colour
         * texture and no re-upload when the highlight moves. */
        out_color = v_color * texel.r;
    } else {
        out_color = texel;
    }
}
