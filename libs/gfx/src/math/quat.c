/*
 * Quaternion (x, y, z, w). w is the scalar part.
 * Rotation by q is v' = q * v * q^-1.
 */
#include <math.h>
#include <optics/gfx/math.h>

optics_gfx_quat optics_gfx_quat_identity(void) {
    return (optics_gfx_quat){0, 0, 0, 1};
}

optics_gfx_quat optics_gfx_quat_axis_angle(optics_gfx_vec3 axis, float radians) {
    optics_gfx_vec3 a = optics_gfx_vec3_normalize(axis);
    float half = radians * 0.5f;
    float s = sinf(half);
    return (optics_gfx_quat){a.x * s, a.y * s, a.z * s, cosf(half)};
}

optics_gfx_quat optics_gfx_quat_multiply(optics_gfx_quat a, optics_gfx_quat b) {
    return (optics_gfx_quat){
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

optics_gfx_quat optics_gfx_quat_normalize(optics_gfx_quat q) {
    float len2 = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    if (len2 <= 1e-20f)
        return optics_gfx_quat_identity();
    float inv = 1.0f / sqrtf(len2);
    return (optics_gfx_quat){q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

optics_gfx_vec3 optics_gfx_quat_rotate(optics_gfx_quat q, optics_gfx_vec3 v) {
    /* v + 2 * q.xyz x (q.xyz x v + q.w * v) */
    optics_gfx_vec3 qv = {q.x, q.y, q.z};
    optics_gfx_vec3 t = optics_gfx_vec3_cross(qv, v);
    t = (optics_gfx_vec3){t.x + q.w * v.x, t.y + q.w * v.y, t.z + q.w * v.z};
    optics_gfx_vec3 c = optics_gfx_vec3_cross(qv, t);
    return (optics_gfx_vec3){v.x + 2.0f * c.x, v.y + 2.0f * c.y, v.z + 2.0f * c.z};
}

optics_gfx_quat optics_gfx_quat_slerp(optics_gfx_quat a, optics_gfx_quat b, float t) {
    float cos_h = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (cos_h < 0.0f) {
        b.x = -b.x;
        b.y = -b.y;
        b.z = -b.z;
        b.w = -b.w;
        cos_h = -cos_h;
    }
    if (cos_h > 0.9995f) {
        optics_gfx_quat r = {
            a.x + t * (b.x - a.x),
            a.y + t * (b.y - a.y),
            a.z + t * (b.z - a.z),
            a.w + t * (b.w - a.w),
        };
        return optics_gfx_quat_normalize(r);
    }
    float h = acosf(cos_h);
    float s = sinf(h);
    float wa = sinf((1.0f - t) * h) / s;
    float wb = sinf(t * h) / s;
    return (optics_gfx_quat){
        wa * a.x + wb * b.x,
        wa * a.y + wb * b.y,
        wa * a.z + wb * b.z,
        wa * a.w + wb * b.w,
    };
}
