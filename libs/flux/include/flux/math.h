/*
 * flux/math.h — value-type math primitives.
 *
 * No Vulkan dependency. No allocations except via flux_arena.
 *
 * Naming honesty:
 *   flux_mat3x2  2D affine transform (rows × cols).
 *   flux_mat4    general 4×4 (projections, view, model).
 *   flux_quat    unit quaternion.
 * No type punning between dimensions.
 *
 * flux_color is a packed 32-bit value: bits 31..24 = A, 23..16 = R,
 * 15..8 = G, 7..0 = B (premultiplied). The value is portable across
 * endianness — flux_color_unpack is the only sanctioned decoder; do
 * not memcpy the bytes into a GPU buffer expecting a particular
 * component layout.
 */

#ifndef FLUX_MATH_H
#define FLUX_MATH_H

#include <flux/core.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/*  Vectors (value types)                                             */
/* ================================================================== */

typedef struct flux_vec2 {
    float x, y;
} flux_vec2;
typedef struct flux_vec3 {
    float x, y, z;
} flux_vec3;
typedef struct flux_vec4 {
    float x, y, z, w;
} flux_vec4;

FLUX_API flux_vec2 flux_vec2_make(float x, float y);
FLUX_API flux_vec2 flux_vec2_add(flux_vec2 a, flux_vec2 b);
FLUX_API flux_vec2 flux_vec2_sub(flux_vec2 a, flux_vec2 b);
FLUX_API flux_vec2 flux_vec2_scale(flux_vec2 v, float k);
FLUX_API float flux_vec2_dot(flux_vec2 a, flux_vec2 b);
FLUX_API float flux_vec2_length(flux_vec2 v);
FLUX_API flux_vec2 flux_vec2_normalize(flux_vec2 v);

FLUX_API flux_vec3 flux_vec3_make(float x, float y, float z);
FLUX_API flux_vec3 flux_vec3_add(flux_vec3 a, flux_vec3 b);
FLUX_API flux_vec3 flux_vec3_sub(flux_vec3 a, flux_vec3 b);
FLUX_API flux_vec3 flux_vec3_scale(flux_vec3 v, float k);
FLUX_API float flux_vec3_dot(flux_vec3 a, flux_vec3 b);
FLUX_API flux_vec3 flux_vec3_cross(flux_vec3 a, flux_vec3 b);
FLUX_API float flux_vec3_length(flux_vec3 v);
FLUX_API flux_vec3 flux_vec3_normalize(flux_vec3 v);

FLUX_API flux_vec4 flux_vec4_make(float x, float y, float z, float w);
FLUX_API flux_vec4 flux_vec4_add(flux_vec4 a, flux_vec4 b);
FLUX_API flux_vec4 flux_vec4_sub(flux_vec4 a, flux_vec4 b);
FLUX_API flux_vec4 flux_vec4_scale(flux_vec4 v, float k);
FLUX_API float flux_vec4_dot(flux_vec4 a, flux_vec4 b);
FLUX_API float flux_vec4_length(flux_vec4 v);
FLUX_API flux_vec4 flux_vec4_normalize(flux_vec4 v);

/* ================================================================== */
/*  Rect / point                                                      */
/* ================================================================== */

typedef struct flux_point {
    float x, y;
} flux_point;
typedef struct flux_rect {
    float x, y, w, h;
} flux_rect;

/* ================================================================== */
/*  2D affine matrix (3 rows × 2 cols)                                */
/* ================================================================== */

typedef struct flux_mat3x2 {
    float m[6];
} flux_mat3x2;

FLUX_API flux_mat3x2 flux_mat3x2_identity(void);
FLUX_API flux_mat3x2 flux_mat3x2_translate(float x, float y);
FLUX_API flux_mat3x2 flux_mat3x2_scale(float x, float y);
FLUX_API flux_mat3x2 flux_mat3x2_rotate(float radians);
FLUX_API flux_mat3x2 flux_mat3x2_multiply(flux_mat3x2 a, flux_mat3x2 b);
FLUX_API flux_mat3x2 flux_mat3x2_invert(flux_mat3x2 m);
FLUX_API bool flux_mat3x2_is_identity(flux_mat3x2 m);
FLUX_API flux_point flux_mat3x2_transform_point(flux_mat3x2 m, flux_point p);
FLUX_API flux_rect flux_mat3x2_transform_rect(flux_mat3x2 m, flux_rect r);

/* ================================================================== */
/*  Quaternion (defined before mat4 because mat4 references it)       */
/* ================================================================== */

typedef struct flux_quat {
    float x, y, z, w;
} flux_quat;

/* ================================================================== */
/*  4×4 matrix                                                        */
/* ================================================================== */

typedef struct flux_mat4 {
    float m[16];
} flux_mat4;

FLUX_API flux_mat4 flux_mat4_identity(void);
FLUX_API flux_mat4 flux_mat4_translate(float x, float y, float z);
FLUX_API flux_mat4 flux_mat4_scale(float x, float y, float z);
FLUX_API flux_mat4 flux_mat4_rotation_quat(flux_quat q);
FLUX_API flux_mat4 flux_mat4_multiply(flux_mat4 a, flux_mat4 b);
FLUX_API flux_vec4 flux_mat4_transform_vec4(flux_mat4 m, flux_vec4 v);
FLUX_API flux_mat4 flux_mat4_invert(flux_mat4 m);
FLUX_API flux_mat4 flux_mat4_perspective(float fov_y_rad, float aspect, float z_near, float z_far);
FLUX_API flux_mat4 flux_mat4_orthographic(float left, float right, float bottom, float top,
                                          float z_near, float z_far);
FLUX_API flux_mat4 flux_mat4_look_at(flux_vec3 eye, flux_vec3 center, flux_vec3 up);

/* ================================================================== */
/*  Quaternion operations                                             */
/* ================================================================== */

FLUX_API flux_quat flux_quat_identity(void);
FLUX_API flux_quat flux_quat_axis_angle(flux_vec3 axis, float radians);
FLUX_API flux_quat flux_quat_multiply(flux_quat a, flux_quat b);
FLUX_API flux_quat flux_quat_normalize(flux_quat q);
FLUX_API flux_vec3 flux_quat_rotate(flux_quat q, flux_vec3 v);
FLUX_API flux_quat flux_quat_slerp(flux_quat a, flux_quat b, float t);

/* ================================================================== */
/*  Colour — packed BGRA premultiplied, 8-bit per channel             */
/* ================================================================== */

typedef uint32_t flux_color;

FLUX_API flux_color flux_color_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
FLUX_API flux_color flux_color_rgba_premul(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
FLUX_API void flux_color_unpack(flux_color c, uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *a);
FLUX_API flux_vec4 flux_color_to_linear(flux_color c);
FLUX_API flux_color flux_color_from_linear(flux_vec4 linear);

/* ================================================================== */
/*  Arena allocator (CPU-side bump)                                   */
/* ================================================================== */

typedef struct flux_arena {
    uint8_t *base;
    size_t capacity;
    size_t used;
    flux_allocator alloc;
    bool owns_buffer;
} flux_arena;

FLUX_NODISCARD FLUX_API flux_result flux_arena_init(flux_arena *a, size_t capacity,
                                                    const flux_allocator *alloc /* nullable */);

FLUX_API void flux_arena_destroy(flux_arena *a);
FLUX_API void *flux_arena_alloc(flux_arena *a, size_t bytes);
FLUX_API void *flux_arena_alloc_aligned(flux_arena *a, size_t bytes, size_t align);
FLUX_API void flux_arena_reset(flux_arena *a);

#ifdef __cplusplus
}
#endif

#endif /* FLUX_MATH_H */
