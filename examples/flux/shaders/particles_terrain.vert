#version 450

/*
 * particles_terrain — animated heightfield as a point cloud.
 *
 * A flat grid of particles is generated procedurally from gl_VertexIndex
 * (no vertex buffer). Each particle's x/z is fixed in a square; its y is
 * the sum of a few travelling sinusoids — so the flat plane develops
 * moving peaks and valleys. Per-point colour tracks the height, and the
 * point size grows on the peaks, giving a glowing-terrain feel without
 * any triangle mesh.
 */

layout(push_constant) uniform PC {
    mat4  mvp;        /* projection * view * world */
    float time;       /* seconds since start                */
    float point_size; /* base point size in pixels          */
} pc;

/* Grid resolution. GRID * GRID points, indexed 0..GRID*GRID-1.
 * Kept here (not a push constant) so it matches the host constant. */
const int   GRID = 256;
const float EXT  = 10.0;   /* half-extent of the plane (±EXT on x,z) */

layout(location = 0) out vec3 v_color;
layout(location = 1) out float v_size;

/* Cheap value-noise-free travelling wave field.
 *   y = Σ Aᵢ sin( fᵢ·p + ωᵢ·t )
 * Phase offsets keep the terms incoherent so ridges travel diagonally. */
float height(vec2 p, float t)
{
    float h = 0.0;
    h += 1.20 * sin( p.x * 0.45 + t * 0.80);
    h += 1.00 * sin( p.y * 0.50 - t * 0.65);
    h += 0.55 * sin((p.x + p.y) * 0.33 + t * 1.10);
    h += 0.35 * sin((p.x - p.y) * 0.70 - t * 1.40);
    return h;
}

void main()
{
    int ix = gl_VertexIndex % GRID;
    int iz = gl_VertexIndex / GRID;

    /* Normalised (u,v) in [0,1], centred to [-EXT, +EXT]. */
    vec2 uv = vec2(float(ix), float(iz)) / float(GRID - 1);
    vec2 p  = (uv - 0.5) * (2.0 * EXT);

    float y = height(p, pc.time);

    vec4 world = vec4(p.x, y, p.y, 1.0);
    gl_Position = pc.mvp * world;

    /* Height-mapped colour: deep valleys cool/blue, peaks warm/white.
     * Matches the additive blend — overlap on ridges blows out to white. */
    float hn = clamp((y + 2.0) / 4.0, 0.0, 1.0);
    vec3 cool = vec3(0.10, 0.30, 0.70);
    vec3 warm = vec3(0.95, 0.55, 0.20);
    vec3 hot  = vec3(1.00, 0.95, 0.85);
    v_color = hn < 0.5
        ? mix(cool, warm, hn * 2.0)
        : mix(warm, hot, (hn - 0.5) * 2.0);

    /* Larger, brighter points ride the peaks so crests read as ridges. */
    v_size = pc.point_size * (1.0 + hn * 1.5);
    gl_PointSize = v_size;
}
