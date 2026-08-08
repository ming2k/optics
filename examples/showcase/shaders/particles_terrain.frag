#version 450

/*
 * particles_terrain fragment — soft, additive particle splats.
 *
 * For a POINT_LIST pipeline Vulkan rasterises each point as a square
 * covering gl_PointSize pixels; gl_PointCoord gives the in-primitive
 * uv in [0,1]. We fade the square into a round, soft-edged disc so
 * overlapping points accumulate as a glowing cloud under additive
 * blending instead of looking like a sheet of hard squares.
 */

layout(location = 0) in vec3  v_color;
layout(location = 1) in float v_size;

layout(location = 0) out vec4 out_color;

void main()
{
    /* Distance from the point centre in [0, ~0.707]; fade to zero at the
     * edge so points blend smoothly into their neighbours. */
    vec2  d  = gl_PointCoord - 0.5;
    float r2 = dot(d, d);                /* 0 at centre, 0.25 at corner */
    if (r2 > 0.25) discard;              /* keep it circular           */

    /* Smooth falloff (gaussian-ish). Smaller points glow harder. */
    float falloff = exp(-r2 * 14.0);
    float alpha   = falloff;

    out_color = vec4(v_color * alpha, alpha);
}
