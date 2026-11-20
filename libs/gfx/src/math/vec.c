#include <math.h>
#include <optics/gfx/math.h>

/* ================================================================== */
/*  vec2                                                              */
/* ================================================================== */

optics_gfx_vec2 optics_gfx_vec2_make(float x, float y) {
    return (optics_gfx_vec2){x, y};
}

optics_gfx_vec2 optics_gfx_vec2_add(optics_gfx_vec2 a, optics_gfx_vec2 b) {
    return (optics_gfx_vec2){a.x + b.x, a.y + b.y};
}

optics_gfx_vec2 optics_gfx_vec2_sub(optics_gfx_vec2 a, optics_gfx_vec2 b) {
    return (optics_gfx_vec2){a.x - b.x, a.y - b.y};
}

optics_gfx_vec2 optics_gfx_vec2_scale(optics_gfx_vec2 v, float k) {
    return (optics_gfx_vec2){v.x * k, v.y * k};
}

float optics_gfx_vec2_dot(optics_gfx_vec2 a, optics_gfx_vec2 b) {
    return a.x * b.x + a.y * b.y;
}

float optics_gfx_vec2_length(optics_gfx_vec2 v) {
    return sqrtf(v.x * v.x + v.y * v.y);
}

optics_gfx_vec2 optics_gfx_vec2_normalize(optics_gfx_vec2 v) {
    float len = optics_gfx_vec2_length(v);
    if (len <= 1e-20f)
        return (optics_gfx_vec2){0, 0};
    float inv = 1.0f / len;
    return (optics_gfx_vec2){v.x * inv, v.y * inv};
}

/* ================================================================== */
/*  vec3                                                              */
/* ================================================================== */

optics_gfx_vec3 optics_gfx_vec3_make(float x, float y, float z) {
    return (optics_gfx_vec3){x, y, z};
}

optics_gfx_vec3 optics_gfx_vec3_add(optics_gfx_vec3 a, optics_gfx_vec3 b) {
    return (optics_gfx_vec3){a.x + b.x, a.y + b.y, a.z + b.z};
}

optics_gfx_vec3 optics_gfx_vec3_sub(optics_gfx_vec3 a, optics_gfx_vec3 b) {
    return (optics_gfx_vec3){a.x - b.x, a.y - b.y, a.z - b.z};
}

optics_gfx_vec3 optics_gfx_vec3_scale(optics_gfx_vec3 v, float k) {
    return (optics_gfx_vec3){v.x * k, v.y * k, v.z * k};
}

float optics_gfx_vec3_dot(optics_gfx_vec3 a, optics_gfx_vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

optics_gfx_vec3 optics_gfx_vec3_cross(optics_gfx_vec3 a, optics_gfx_vec3 b) {
    return (optics_gfx_vec3){
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

float optics_gfx_vec3_length(optics_gfx_vec3 v) {
    return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
}

optics_gfx_vec3 optics_gfx_vec3_normalize(optics_gfx_vec3 v) {
    float len = optics_gfx_vec3_length(v);
    if (len <= 1e-20f)
        return (optics_gfx_vec3){0, 0, 0};
    float inv = 1.0f / len;
    return (optics_gfx_vec3){v.x * inv, v.y * inv, v.z * inv};
}

/* ================================================================== */
/*  vec4                                                              */
/* ================================================================== */

optics_gfx_vec4 optics_gfx_vec4_make(float x, float y, float z, float w) {
    return (optics_gfx_vec4){x, y, z, w};
}

optics_gfx_vec4 optics_gfx_vec4_add(optics_gfx_vec4 a, optics_gfx_vec4 b) {
    return (optics_gfx_vec4){a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}

optics_gfx_vec4 optics_gfx_vec4_sub(optics_gfx_vec4 a, optics_gfx_vec4 b) {
    return (optics_gfx_vec4){a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
}

optics_gfx_vec4 optics_gfx_vec4_scale(optics_gfx_vec4 v, float k) {
    return (optics_gfx_vec4){v.x * k, v.y * k, v.z * k, v.w * k};
}

float optics_gfx_vec4_dot(optics_gfx_vec4 a, optics_gfx_vec4 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

float optics_gfx_vec4_length(optics_gfx_vec4 v) {
    return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z + v.w * v.w);
}

optics_gfx_vec4 optics_gfx_vec4_normalize(optics_gfx_vec4 v) {
    float len = optics_gfx_vec4_length(v);
    if (len <= 1e-20f)
        return (optics_gfx_vec4){0, 0, 0, 0};
    float inv = 1.0f / len;
    return (optics_gfx_vec4){v.x * inv, v.y * inv, v.z * inv, v.w * inv};
}
