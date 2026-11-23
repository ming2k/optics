/*
 * Matrix math.
 *
 * optics_gfx_mat3x2 (2D affine) layout matches CoreGraphics / Cairo:
 *   [ a c tx ]     m[0] m[2] m[4]
 *   [ b d ty ]  =  m[1] m[3] m[5]
 *   (x, y) -> (a*x + c*y + tx,  b*x + d*y + ty)
 *
 * optics_gfx_mat4 layout is column-major. Indexed as m[col*4 + row]:
 *   m[ 0 4  8 12]
 *   m[ 1 5  9 13]
 *   m[ 2 6 10 14]
 *   m[ 3 7 11 15]
 *
 * Conventions for projection:
 *   - Right-handed world space.
 *   - Vulkan-style clip: depth in [0, 1], Y axis flipped (negative
 *     m[5]) so that +Y in NDC points down.
 *
 * optics_gfx_mat4_multiply(a, b) is a*b in math notation; a vector
 * transformed by (M*v) applies translation last.
 *
 * Invert behaviour: both inverts return identity on a singular
 * matrix. Pre-check the determinant if you need to distinguish.
 */
#include <math.h>
#include <optics/gfx/math.h>
#include <string.h>

/* Forward declare the core's last-error shim so we can record an
 * INVALID_ARGUMENT diagnostic when an invert falls back to identity.
 * Defined in src/core/result.c. */
void optics_gfx_set_last_error(optics_gfx_result code, const char *function, const char *file,
                               int line, const char *message, int32_t backend_code);

/* ================================================================== */
/*  mat3x2                                                            */
/* ================================================================== */

optics_gfx_mat3x2 optics_gfx_mat3x2_identity(void) {
    return (optics_gfx_mat3x2){{1, 0, 0, 1, 0, 0}};
}

optics_gfx_mat3x2 optics_gfx_mat3x2_translate(float x, float y) {
    return (optics_gfx_mat3x2){{1, 0, 0, 1, x, y}};
}

optics_gfx_mat3x2 optics_gfx_mat3x2_scale(float x, float y) {
    return (optics_gfx_mat3x2){{x, 0, 0, y, 0, 0}};
}

optics_gfx_mat3x2 optics_gfx_mat3x2_rotate(float radians) {
    float c = cosf(radians);
    float s = sinf(radians);
    return (optics_gfx_mat3x2){{c, s, -s, c, 0, 0}};
}

optics_gfx_mat3x2 optics_gfx_mat3x2_multiply(optics_gfx_mat3x2 a, optics_gfx_mat3x2 b) {
    return (optics_gfx_mat3x2){{
        a.m[0] * b.m[0] + a.m[2] * b.m[1],
        a.m[1] * b.m[0] + a.m[3] * b.m[1],
        a.m[0] * b.m[2] + a.m[2] * b.m[3],
        a.m[1] * b.m[2] + a.m[3] * b.m[3],
        a.m[0] * b.m[4] + a.m[2] * b.m[5] + a.m[4],
        a.m[1] * b.m[4] + a.m[3] * b.m[5] + a.m[5],
    }};
}

optics_gfx_mat3x2 optics_gfx_mat3x2_invert(optics_gfx_mat3x2 m) {
    float det = m.m[0] * m.m[3] - m.m[1] * m.m[2];
    if (fabsf(det) <= 1e-20f) {
        optics_gfx_set_last_error(OPTICS_GFX_ERROR_INVALID_ARGUMENT, __func__, __FILE__, __LINE__,
                                  "singular mat3x2; returning identity", 0);
        return optics_gfx_mat3x2_identity();
    }
    float inv = 1.0f / det;
    return (optics_gfx_mat3x2){{
        m.m[3] * inv,
        -m.m[1] * inv,
        -m.m[2] * inv,
        m.m[0] * inv,
        (m.m[2] * m.m[5] - m.m[3] * m.m[4]) * inv,
        (m.m[1] * m.m[4] - m.m[0] * m.m[5]) * inv,
    }};
}

bool optics_gfx_mat3x2_is_identity(optics_gfx_mat3x2 m) {
    return m.m[0] == 1.0f && m.m[1] == 0.0f && m.m[2] == 0.0f && m.m[3] == 1.0f && m.m[4] == 0.0f &&
           m.m[5] == 0.0f;
}

optics_gfx_point optics_gfx_mat3x2_transform_point(optics_gfx_mat3x2 m, optics_gfx_point p) {
    return (optics_gfx_point){
        .x = m.m[0] * p.x + m.m[2] * p.y + m.m[4],
        .y = m.m[1] * p.x + m.m[3] * p.y + m.m[5],
    };
}

optics_gfx_rect optics_gfx_mat3x2_transform_rect(optics_gfx_mat3x2 m, optics_gfx_rect r) {
    optics_gfx_point corners[4] = {
        {r.x, r.y},
        {r.x + r.w, r.y},
        {r.x, r.y + r.h},
        {r.x + r.w, r.y + r.h},
    };
    float min_x = INFINITY, min_y = INFINITY;
    float max_x = -INFINITY, max_y = -INFINITY;
    for (int i = 0; i < 4; ++i) {
        optics_gfx_point p = optics_gfx_mat3x2_transform_point(m, corners[i]);
        if (p.x < min_x)
            min_x = p.x;
        if (p.y < min_y)
            min_y = p.y;
        if (p.x > max_x)
            max_x = p.x;
        if (p.y > max_y)
            max_y = p.y;
    }
    return (optics_gfx_rect){
        .x = min_x,
        .y = min_y,
        .w = max_x - min_x,
        .h = max_y - min_y,
    };
}

/* ================================================================== */
/*  mat4                                                              */
/* ================================================================== */

optics_gfx_mat4 optics_gfx_mat4_identity(void) {
    optics_gfx_mat4 m;
    memset(&m, 0, sizeof(m));
    m.m[0] = m.m[5] = m.m[10] = m.m[15] = 1.0f;
    return m;
}

optics_gfx_mat4 optics_gfx_mat4_translate(float x, float y, float z) {
    optics_gfx_mat4 m = optics_gfx_mat4_identity();
    m.m[12] = x;
    m.m[13] = y;
    m.m[14] = z;
    return m;
}

optics_gfx_mat4 optics_gfx_mat4_scale(float x, float y, float z) {
    optics_gfx_mat4 m;
    memset(&m, 0, sizeof(m));
    m.m[0] = x;
    m.m[5] = y;
    m.m[10] = z;
    m.m[15] = 1.0f;
    return m;
}

optics_gfx_mat4 optics_gfx_mat4_rotation_quat(optics_gfx_quat q) {
    float x = q.x, y = q.y, z = q.z, w = q.w;
    float xx = x * x, yy = y * y, zz = z * z;
    float xy = x * y, xz = x * z, yz = y * z;
    float wx = w * x, wy = w * y, wz = w * z;

    optics_gfx_mat4 m;
    m.m[0] = 1.0f - 2.0f * (yy + zz);
    m.m[1] = 2.0f * (xy + wz);
    m.m[2] = 2.0f * (xz - wy);
    m.m[3] = 0.0f;
    m.m[4] = 2.0f * (xy - wz);
    m.m[5] = 1.0f - 2.0f * (xx + zz);
    m.m[6] = 2.0f * (yz + wx);
    m.m[7] = 0.0f;
    m.m[8] = 2.0f * (xz + wy);
    m.m[9] = 2.0f * (yz - wx);
    m.m[10] = 1.0f - 2.0f * (xx + yy);
    m.m[11] = 0.0f;
    m.m[12] = m.m[13] = m.m[14] = 0.0f;
    m.m[15] = 1.0f;
    return m;
}

optics_gfx_mat4 optics_gfx_mat4_multiply(optics_gfx_mat4 a, optics_gfx_mat4 b) {
    optics_gfx_mat4 r;
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) {
                s += a.m[k * 4 + row] * b.m[col * 4 + k];
            }
            r.m[col * 4 + row] = s;
        }
    }
    return r;
}

optics_gfx_vec4 optics_gfx_mat4_transform_vec4(optics_gfx_mat4 m, optics_gfx_vec4 v) {
    return (optics_gfx_vec4){
        .x = m.m[0] * v.x + m.m[4] * v.y + m.m[8] * v.z + m.m[12] * v.w,
        .y = m.m[1] * v.x + m.m[5] * v.y + m.m[9] * v.z + m.m[13] * v.w,
        .z = m.m[2] * v.x + m.m[6] * v.y + m.m[10] * v.z + m.m[14] * v.w,
        .w = m.m[3] * v.x + m.m[7] * v.y + m.m[11] * v.z + m.m[15] * v.w,
    };
}

optics_gfx_mat4 optics_gfx_mat4_invert(optics_gfx_mat4 m) {
    const float *a = m.m;
    float inv[16];

    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] +
             a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] -
             a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] +
             a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] -
              a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];

    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] -
             a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] +
             a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] -
             a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] +
              a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];

    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] +
             a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] -
             a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] +
              a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] -
              a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];

    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] -
             a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] +
             a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] -
              a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] +
              a[8] * a[1] * a[6] - a[8] * a[2] * a[5];

    float det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (fabsf(det) <= 1e-20f) {
        optics_gfx_set_last_error(OPTICS_GFX_ERROR_INVALID_ARGUMENT, __func__, __FILE__, __LINE__,
                                  "singular mat4; returning identity", 0);
        return optics_gfx_mat4_identity();
    }

    float inv_det = 1.0f / det;
    optics_gfx_mat4 out;
    for (int i = 0; i < 16; ++i)
        out.m[i] = inv[i] * inv_det;
    return out;
}

optics_gfx_mat4 optics_gfx_mat4_perspective(float fov_y_rad, float aspect, float z_near,
                                            float z_far) {
    float f = 1.0f / tanf(fov_y_rad * 0.5f);
    optics_gfx_mat4 m;
    memset(&m, 0, sizeof(m));
    m.m[0] = f / aspect;
    m.m[5] = -f; /* Vulkan +Y down */
    m.m[10] = z_far / (z_near - z_far);
    m.m[11] = -1.0f;
    m.m[14] = (z_near * z_far) / (z_near - z_far);
    return m;
}

optics_gfx_mat4 optics_gfx_mat4_orthographic(float left, float right, float bottom, float top,
                                             float z_near, float z_far) {
    optics_gfx_mat4 m;
    memset(&m, 0, sizeof(m));
    m.m[0] = 2.0f / (right - left);
    m.m[5] = -2.0f / (top - bottom); /* Vulkan +Y down */
    m.m[10] = -1.0f / (z_far - z_near);
    m.m[12] = -(right + left) / (right - left);
    m.m[13] = (top + bottom) / (top - bottom);
    m.m[14] = -z_near / (z_far - z_near);
    m.m[15] = 1.0f;
    return m;
}

optics_gfx_mat4 optics_gfx_mat4_look_at(optics_gfx_vec3 eye, optics_gfx_vec3 center,
                                        optics_gfx_vec3 up) {
    optics_gfx_vec3 f = optics_gfx_vec3_normalize(optics_gfx_vec3_sub(center, eye));
    optics_gfx_vec3 s = optics_gfx_vec3_normalize(optics_gfx_vec3_cross(f, up));
    optics_gfx_vec3 u = optics_gfx_vec3_cross(s, f);

    optics_gfx_mat4 m = optics_gfx_mat4_identity();
    m.m[0] = s.x;
    m.m[4] = s.y;
    m.m[8] = s.z;
    m.m[1] = u.x;
    m.m[5] = u.y;
    m.m[9] = u.z;
    m.m[2] = -f.x;
    m.m[6] = -f.y;
    m.m[10] = -f.z;
    m.m[12] = -optics_gfx_vec3_dot(s, eye);
    m.m[13] = -optics_gfx_vec3_dot(u, eye);
    m.m[14] = optics_gfx_vec3_dot(f, eye);
    return m;
}

/* ================================================================== */
/*  mat3 (color-space transforms; column-major like optics_gfx_mat4)        */
/* ================================================================== */

optics_gfx_mat3 optics_gfx_mat3_identity(void) {
    return (optics_gfx_mat3){{1, 0, 0, 0, 1, 0, 0, 0, 1}};
}

optics_gfx_mat3 optics_gfx_mat3_multiply(optics_gfx_mat3 a, optics_gfx_mat3 b) {
    optics_gfx_mat3 r;
    for (int col = 0; col < 3; ++col) {
        for (int row = 0; row < 3; ++row) {
            r.m[col * 3 + row] = a.m[row] * b.m[col * 3] + a.m[3 + row] * b.m[col * 3 + 1] +
                                 a.m[6 + row] * b.m[col * 3 + 2];
        }
    }
    return r;
}

optics_gfx_vec3 optics_gfx_mat3_transform_vec3(optics_gfx_mat3 m, optics_gfx_vec3 v) {
    return (optics_gfx_vec3){
        m.m[0] * v.x + m.m[3] * v.y + m.m[6] * v.z,
        m.m[1] * v.x + m.m[4] * v.y + m.m[7] * v.z,
        m.m[2] * v.x + m.m[5] * v.y + m.m[8] * v.z,
    };
}

optics_gfx_mat3 optics_gfx_mat3_invert(optics_gfx_mat3 m) {
    /* A(r, c) reads the row-major view of the column-major storage. The
     * matrix parameter is named `a`, not `m`: the preprocessor substitutes
     * parameters by token, so a parameter named `m` would also rewrite the
     * `.m` member access. */
#define OPTICS_GFX_M3(a, r, c) (a).m[(c) * 3 + (r)]
    /* Cofactor matrix. */
    float c00 = +(OPTICS_GFX_M3(m, 1, 1) * OPTICS_GFX_M3(m, 2, 2) -
                  OPTICS_GFX_M3(m, 1, 2) * OPTICS_GFX_M3(m, 2, 1));
    float c01 = -(OPTICS_GFX_M3(m, 1, 0) * OPTICS_GFX_M3(m, 2, 2) -
                  OPTICS_GFX_M3(m, 1, 2) * OPTICS_GFX_M3(m, 2, 0));
    float c02 = +(OPTICS_GFX_M3(m, 1, 0) * OPTICS_GFX_M3(m, 2, 1) -
                  OPTICS_GFX_M3(m, 1, 1) * OPTICS_GFX_M3(m, 2, 0));
    float c10 = -(OPTICS_GFX_M3(m, 0, 1) * OPTICS_GFX_M3(m, 2, 2) -
                  OPTICS_GFX_M3(m, 0, 2) * OPTICS_GFX_M3(m, 2, 1));
    float c11 = +(OPTICS_GFX_M3(m, 0, 0) * OPTICS_GFX_M3(m, 2, 2) -
                  OPTICS_GFX_M3(m, 0, 2) * OPTICS_GFX_M3(m, 2, 0));
    float c12 = -(OPTICS_GFX_M3(m, 0, 0) * OPTICS_GFX_M3(m, 2, 1) -
                  OPTICS_GFX_M3(m, 0, 1) * OPTICS_GFX_M3(m, 2, 0));
    float c20 = +(OPTICS_GFX_M3(m, 0, 1) * OPTICS_GFX_M3(m, 1, 2) -
                  OPTICS_GFX_M3(m, 0, 2) * OPTICS_GFX_M3(m, 1, 1));
    float c21 = -(OPTICS_GFX_M3(m, 0, 0) * OPTICS_GFX_M3(m, 1, 2) -
                  OPTICS_GFX_M3(m, 0, 2) * OPTICS_GFX_M3(m, 1, 0));
    float c22 = +(OPTICS_GFX_M3(m, 0, 0) * OPTICS_GFX_M3(m, 1, 1) -
                  OPTICS_GFX_M3(m, 0, 1) * OPTICS_GFX_M3(m, 1, 0));
    float det =
        OPTICS_GFX_M3(m, 0, 0) * c00 + OPTICS_GFX_M3(m, 0, 1) * c01 + OPTICS_GFX_M3(m, 0, 2) * c02;
    if (fabsf(det) <= 1e-20f) {
        optics_gfx_set_last_error(OPTICS_GFX_ERROR_INVALID_ARGUMENT, __func__, __FILE__, __LINE__,
                                  "singular mat3; returning identity", 0);
        return optics_gfx_mat3_identity();
    }
    float inv = 1.0f / det;
    /* inverse = adjugate / det, the adjugate being the cofactor transpose. */
    optics_gfx_mat3 r;
    OPTICS_GFX_M3(r, 0, 0) = c00 * inv;
    OPTICS_GFX_M3(r, 0, 1) = c10 * inv;
    OPTICS_GFX_M3(r, 0, 2) = c20 * inv;
    OPTICS_GFX_M3(r, 1, 0) = c01 * inv;
    OPTICS_GFX_M3(r, 1, 1) = c11 * inv;
    OPTICS_GFX_M3(r, 1, 2) = c21 * inv;
    OPTICS_GFX_M3(r, 2, 0) = c02 * inv;
    OPTICS_GFX_M3(r, 2, 1) = c12 * inv;
    OPTICS_GFX_M3(r, 2, 2) = c22 * inv;
    return r;
#undef OPTICS_GFX_M3
}
