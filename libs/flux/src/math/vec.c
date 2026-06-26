#include <flux/math.h>
#include <math.h>

/* ================================================================== */
/*  vec2                                                              */
/* ================================================================== */

flux_vec2 flux_vec2_make(float x, float y) {
    return (flux_vec2){x, y};
}

flux_vec2 flux_vec2_add(flux_vec2 a, flux_vec2 b) {
    return (flux_vec2){a.x + b.x, a.y + b.y};
}

flux_vec2 flux_vec2_sub(flux_vec2 a, flux_vec2 b) {
    return (flux_vec2){a.x - b.x, a.y - b.y};
}

flux_vec2 flux_vec2_scale(flux_vec2 v, float k) {
    return (flux_vec2){v.x * k, v.y * k};
}

float flux_vec2_dot(flux_vec2 a, flux_vec2 b) {
    return a.x * b.x + a.y * b.y;
}

float flux_vec2_length(flux_vec2 v) {
    return sqrtf(v.x * v.x + v.y * v.y);
}

flux_vec2 flux_vec2_normalize(flux_vec2 v) {
    float len = flux_vec2_length(v);
    if (len <= 1e-20f)
        return (flux_vec2){0, 0};
    float inv = 1.0f / len;
    return (flux_vec2){v.x * inv, v.y * inv};
}

/* ================================================================== */
/*  vec3                                                              */
/* ================================================================== */

flux_vec3 flux_vec3_make(float x, float y, float z) {
    return (flux_vec3){x, y, z};
}

flux_vec3 flux_vec3_add(flux_vec3 a, flux_vec3 b) {
    return (flux_vec3){a.x + b.x, a.y + b.y, a.z + b.z};
}

flux_vec3 flux_vec3_sub(flux_vec3 a, flux_vec3 b) {
    return (flux_vec3){a.x - b.x, a.y - b.y, a.z - b.z};
}

flux_vec3 flux_vec3_scale(flux_vec3 v, float k) {
    return (flux_vec3){v.x * k, v.y * k, v.z * k};
}

float flux_vec3_dot(flux_vec3 a, flux_vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

flux_vec3 flux_vec3_cross(flux_vec3 a, flux_vec3 b) {
    return (flux_vec3){
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

float flux_vec3_length(flux_vec3 v) {
    return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
}

flux_vec3 flux_vec3_normalize(flux_vec3 v) {
    float len = flux_vec3_length(v);
    if (len <= 1e-20f)
        return (flux_vec3){0, 0, 0};
    float inv = 1.0f / len;
    return (flux_vec3){v.x * inv, v.y * inv, v.z * inv};
}

/* ================================================================== */
/*  vec4                                                              */
/* ================================================================== */

flux_vec4 flux_vec4_make(float x, float y, float z, float w) {
    return (flux_vec4){x, y, z, w};
}

flux_vec4 flux_vec4_add(flux_vec4 a, flux_vec4 b) {
    return (flux_vec4){a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}

flux_vec4 flux_vec4_sub(flux_vec4 a, flux_vec4 b) {
    return (flux_vec4){a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
}

flux_vec4 flux_vec4_scale(flux_vec4 v, float k) {
    return (flux_vec4){v.x * k, v.y * k, v.z * k, v.w * k};
}

float flux_vec4_dot(flux_vec4 a, flux_vec4 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

float flux_vec4_length(flux_vec4 v) {
    return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z + v.w * v.w);
}

flux_vec4 flux_vec4_normalize(flux_vec4 v) {
    float len = flux_vec4_length(v);
    if (len <= 1e-20f)
        return (flux_vec4){0, 0, 0, 0};
    float inv = 1.0f / len;
    return (flux_vec4){v.x * inv, v.y * inv, v.z * inv, v.w * inv};
}
