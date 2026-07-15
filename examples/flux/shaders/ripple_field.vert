#version 450

layout(push_constant) uniform PC {
    mat4  mvp;
    float time;
    float point_size;
    float ripple_x;
    float ripple_z;
} pc;

const int   GRID = 200;
const float EXT  = 12.0;

layout(location = 0) out vec3 v_color;
layout(location = 1) out float v_size;

void main()
{
    int ix = gl_VertexIndex % GRID;
    int iz = gl_VertexIndex / GRID;

    vec2 uv = vec2(float(ix), float(iz)) / float(GRID - 1);
    vec2 p  = (uv - 0.5) * (2.0 * EXT);

    // Basic wave
    float distToCenter = length(p);
    float y = sin(distToCenter * 2.0 - pc.time * 2.0) * 0.5;

    // The CPU unprojects the cursor onto the y=0 ground plane.
    vec2 rippleOrigin = vec2(pc.ripple_x, pc.ripple_z);
    float distToRipple = length(p - rippleOrigin);

    // Add ripple effect near the cursor.
    float ripple = 0.0;
    if (distToRipple < 4.0) {
        ripple = cos(distToRipple * 5.0 - pc.time * 10.0) * (4.0 - distToRipple) * 0.2;
    }

    y += ripple;

    vec4 world = vec4(p.x, y, p.y, 1.0);
    gl_Position = pc.mvp * world;

    // Cool, luminous particles with white highlights.
    vec3 baseColor = vec3(0.61, 0.72, 0.81); // #9db8cf
    vec3 highlight = vec3(1.0, 1.0, 1.0);

    float intensity = clamp((y + 0.5) / 1.5, 0.0, 1.0);
    v_color = mix(baseColor, highlight, intensity);

    v_size = pc.point_size * (1.0 + intensity * 2.0 + ripple);
    gl_PointSize = v_size;
}
