#version 450

#extension GL_GOOGLE_include_directive : require

#include "flux_sdf.glsl"

/*
 * Signed-distance-field fragment shader for rounded rectangles and circles.
 *
 * The field itself lives in flux_sdf.glsl (ADR-0118) — one definition, shared
 * with the Prism materials and matching <flux/sdf.h> on the host.
 *
 * Resolution-independent analytic anti-aliasing: the quad covers the shape's
 * bounding box (plus a small fringe), and coverage is computed from the
 * screen-space distance to the shape boundary. Unlike MSAA this stays smooth
 * at any pixel density, including 1x (100% scale).
 *
 * Shape parameters arrive in the push block at the same offsets as the image
 * draw's image_dst / image_src (unused by this pipeline), all in screen pixels:
 *   sdf_box   = (center.x, center.y, half.x, half.y), centre in screen pixels,
 *               half extents in SHAPE units
 *   sdf_param = (corner_radius, stroke_half_width, curvature, shape_units_per_pixel)
 * stroke_half_width <= 0 fills the shape; > 0 draws a centred ring (border).
 * curvature 0 is a circular-corner rounded rectangle; 1 is the L4 "G2" squircle.
 * sdf_inv is the inverse of the transform's linear part, so a fragment is mapped
 * back into shape space and the field is evaluated there — exact for any
 * similarity, which is all this pipeline admits.
 */

layout(location = 0) in  vec4 v_color;   /* premultiplied, like the solid frag */
layout(location = 1) in  vec2 v_pos;     /* screen-pixel position */
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform PC {
    layout(offset = 32)  vec4 sdf_inv;    /* inverse linear: a, b, c, d (row-major) */
    layout(offset = 128) vec4 sdf_box;    /* cx, cy, hx, hy */
    layout(offset = 144) vec4 sdf_param;  /* radius, stroke_hw, curvature, aa */
} pc;

void main()
{
    /* Map the fragment back into the shape's own space (name-shadowed in the
     * shader's manifest via the sdf_inv convention). */
    vec2 rel = v_pos - pc.sdf_box.xy;
    vec2 p   = vec2(pc.sdf_inv.x * rel.x + pc.sdf_inv.y * rel.y,
                    pc.sdf_inv.z * rel.x + pc.sdf_inv.w * rel.y);

    /* Signed distance to the shape (negative inside). A rounded rectangle and
     * a squircle are one field family: curvature 0 is exactly the circular
     * corner, so both go through the same call. */
    float d = flux_sdf_squircle(p, pc.sdf_box.zw, pc.sdf_param.x, pc.sdf_param.z);

    /* Border: turn the filled region into a ring of the given half-width. */
    d = flux_sdf_ring(d, pc.sdf_param.y);

    /* ~1px analytic coverage: the ramp is one screen pixel wide, which is
     * sdf_param.w shape units. */
    float aa  = max(pc.sdf_param.w, 1e-4);
    float cov = clamp(0.5 - d / aa, 0.0, 1.0);

    out_color = v_color * cov;
}
