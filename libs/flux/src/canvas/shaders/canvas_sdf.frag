#version 450

/*
 * Signed-distance-field fragment shader for rounded rectangles and circles.
 *
 * Resolution-independent analytic anti-aliasing: the quad covers the shape's
 * bounding box (plus a small fringe), and coverage is computed from the
 * screen-space distance to the shape boundary. Unlike MSAA this stays smooth
 * at any pixel density, including 1x (100% scale).
 *
 * Shape parameters arrive in the push block at the same offsets as the image
 * draw's image_dst / image_src (unused by this pipeline), all in screen pixels:
 *   sdf_box   = (center.x, center.y, half.x, half.y)
 *   sdf_param = (corner_radius, stroke_half_width, 0, 0)
 * stroke_half_width <= 0 fills the shape; > 0 draws a centred ring (border).
 */

layout(location = 0) in  vec4 v_color;   /* premultiplied, like the solid frag */
layout(location = 1) in  vec2 v_pos;     /* screen-pixel position */
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform PC {
    layout(offset = 128) vec4 sdf_box;    /* cx, cy, hx, hy */
    layout(offset = 144) vec4 sdf_param;  /* radius, stroke_hw, _, _ */
} pc;

void main()
{
    float radius = pc.sdf_param.x;
    vec2  p      = v_pos - pc.sdf_box.xy;
    vec2  q      = abs(p) - (pc.sdf_box.zw - vec2(radius));
    /* Signed distance to a rounded rect (negative inside). */
    float d = min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - radius;

    /* Border: turn the filled region into a ring of the given half-width. */
    float stroke_hw = pc.sdf_param.y;
    if (stroke_hw > 0.0) {
        d = abs(d) - stroke_hw;
    }

    /* ~1px analytic coverage from the screen-space gradient of the field. */
    float aa  = max(fwidth(d), 1e-4);
    float cov = clamp(0.5 - d / aa, 0.0, 1.0);

    out_color = v_color * cov;
}
