#version 450

layout(location = 0) in vec3  v_color;
layout(location = 1) in float v_size;

layout(location = 0) out vec4 out_color;

void main()
{
    vec2  d  = gl_PointCoord - 0.5;
    float r2 = dot(d, d);
    if (r2 > 0.25) discard;

    float falloff = exp(-r2 * 10.0);
    float alpha   = falloff;

    out_color = vec4(v_color * alpha, alpha);
}
