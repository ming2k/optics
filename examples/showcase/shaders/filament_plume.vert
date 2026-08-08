#version 450

/*
 * Direct GLSL translation of the 140-byte p5.js / dwitter formula used by
 * filament_plume. Every vertex is one of the original 10,000 points; the point
 * index replaces the JavaScript loop variable `i`, so no vertex buffer is
 * needed.
 */

layout(push_constant) uniform PC {
    vec2  extent;
    float time;
    float point_size;
} pc;

void main()
{
    float i = float(gl_VertexIndex);
    float y = i / 235.0;

    float k = (4.0 + cos(i / 9.0 - pc.time * 2.0)) * cos(i / 35.0);
    float e = y / 7.0 - 13.0;
    float d = length(vec2(k, e)) + sin(e / 9.0 + pc.time / 2.0) - 4.0;
    float q = 2.0 * sin(k * 3.0)
            - y / 35.0 * k * (9.0 + k * sin(cos(e) * 9.0 - d * 2.0 + pc.time));
    float c = d - pc.time;

    /* Original p5.js point in its 400x400 canvas. */
    vec2 point_px = vec2(q + 40.0 * cos(c) + 200.0,
                         q * sin(c) + d * 35.0);

    /* Preserve the square composition in a resizable window. */
    float scale = min(pc.extent.x, pc.extent.y) / 400.0;
    vec2 screen_px = (point_px - vec2(200.0)) * scale + pc.extent * 0.5;
    vec2 ndc = screen_px / pc.extent * 2.0 - 1.0;

    gl_Position = vec4(ndc, 0.0, 1.0);
    gl_PointSize = pc.point_size;
}
