/*
 * gfx/math.h — value-type math primitives.
 *
 * No Vulkan dependency. No allocations except via optics_gfx_arena.
 *
 * Naming honesty:
 *   optics_gfx_mat3x2  2D affine transform (rows × cols).
 *   optics_gfx_mat4    general 4×4 (projections, view, model).
 *   optics_gfx_quat    unit quaternion.
 * No type punning between dimensions.
 *
 * optics_gfx_color is a packed 32-bit value: bits 31..24 = A, 23..16 = R,
 * 15..8 = G, 7..0 = B (premultiplied). The value is portable across
 * endianness — optics_gfx_color_unpack is the only sanctioned decoder; do
 * not memcpy the bytes into a GPU buffer expecting a particular
 * component layout.
 */

#ifndef OPTICS_GFX_MATH_H
#define OPTICS_GFX_MATH_H

#include <optics/gfx/core.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/*  Vectors (value types)                                             */
/* ================================================================== */

typedef struct optics_gfx_vec2 {
    float x, y;
} optics_gfx_vec2;
typedef struct optics_gfx_vec3 {
    float x, y, z;
} optics_gfx_vec3;
typedef struct optics_gfx_vec4 {
    float x, y, z, w;
} optics_gfx_vec4;

OPTICS_GFX_API optics_gfx_vec2 optics_gfx_vec2_make(float x, float y);
OPTICS_GFX_API optics_gfx_vec2 optics_gfx_vec2_add(optics_gfx_vec2 a, optics_gfx_vec2 b);
OPTICS_GFX_API optics_gfx_vec2 optics_gfx_vec2_sub(optics_gfx_vec2 a, optics_gfx_vec2 b);
OPTICS_GFX_API optics_gfx_vec2 optics_gfx_vec2_scale(optics_gfx_vec2 v, float k);
OPTICS_GFX_API float optics_gfx_vec2_dot(optics_gfx_vec2 a, optics_gfx_vec2 b);
OPTICS_GFX_API float optics_gfx_vec2_length(optics_gfx_vec2 v);
OPTICS_GFX_API optics_gfx_vec2 optics_gfx_vec2_normalize(optics_gfx_vec2 v);

OPTICS_GFX_API optics_gfx_vec3 optics_gfx_vec3_make(float x, float y, float z);
OPTICS_GFX_API optics_gfx_vec3 optics_gfx_vec3_add(optics_gfx_vec3 a, optics_gfx_vec3 b);
OPTICS_GFX_API optics_gfx_vec3 optics_gfx_vec3_sub(optics_gfx_vec3 a, optics_gfx_vec3 b);
OPTICS_GFX_API optics_gfx_vec3 optics_gfx_vec3_scale(optics_gfx_vec3 v, float k);
OPTICS_GFX_API float optics_gfx_vec3_dot(optics_gfx_vec3 a, optics_gfx_vec3 b);
OPTICS_GFX_API optics_gfx_vec3 optics_gfx_vec3_cross(optics_gfx_vec3 a, optics_gfx_vec3 b);
OPTICS_GFX_API float optics_gfx_vec3_length(optics_gfx_vec3 v);
OPTICS_GFX_API optics_gfx_vec3 optics_gfx_vec3_normalize(optics_gfx_vec3 v);

OPTICS_GFX_API optics_gfx_vec4 optics_gfx_vec4_make(float x, float y, float z, float w);
OPTICS_GFX_API optics_gfx_vec4 optics_gfx_vec4_add(optics_gfx_vec4 a, optics_gfx_vec4 b);
OPTICS_GFX_API optics_gfx_vec4 optics_gfx_vec4_sub(optics_gfx_vec4 a, optics_gfx_vec4 b);
OPTICS_GFX_API optics_gfx_vec4 optics_gfx_vec4_scale(optics_gfx_vec4 v, float k);
OPTICS_GFX_API float optics_gfx_vec4_dot(optics_gfx_vec4 a, optics_gfx_vec4 b);
OPTICS_GFX_API float optics_gfx_vec4_length(optics_gfx_vec4 v);
OPTICS_GFX_API optics_gfx_vec4 optics_gfx_vec4_normalize(optics_gfx_vec4 v);

/* ================================================================== */
/*  Rect / point                                                      */
/* ================================================================== */

typedef struct optics_gfx_point {
    float x, y;
} optics_gfx_point;

/* Per-corner radii in clockwise order: top-left, top-right, bottom-right,
 * bottom-left (CSS border-radius / Skia SkRRect convention). */
typedef struct optics_gfx_corner_radii {
    float tl, tr, br, bl;
} optics_gfx_corner_radii;

static inline optics_gfx_corner_radii optics_gfx_corner_radii_uniform(float r) {
    return (optics_gfx_corner_radii){r, r, r, r};
}

static inline optics_gfx_corner_radii optics_gfx_corner_radii_all(float tl, float tr, float br,
                                                                  float bl) {
    return (optics_gfx_corner_radii){tl, tr, br, bl};
}

/* CSS Backgrounds & Borders Level 3 / Skia proportional radius normalization:
 * clamps negative/non-finite entries to zero and, when adjacent corners exceed
 * their shared edge, scales all four radii by the smallest factor that makes
 * every edge fit. A single corner may claim up to min(width, height). */
OPTICS_GFX_API optics_gfx_corner_radii
optics_gfx_corner_radii_normalize(optics_gfx_corner_radii radii, float width, float height);

/* optics_gfx_rect is a core value type (gfx/core.h): damage is a frame-native
 * concept and every layer that names a dirty region needs the same struct. */

/* The smallest rectangle covering both `a` and `b`. An empty operand
 * contributes nothing; two empty rectangles union to an empty rectangle.
 *
 * This is the one *lossy* projection in the damage vocabulary, and it exists for
 * exactly one case: a backend whose presentation API accepts a single damage
 * rectangle (Win32 `DwmFlush`-style regions, most Vulkan `renderArea`s). A
 * producer that can describe a set must never union before the consumer has had
 * the chance to use the set — the union of a top strip and a bottom dock is the
 * whole output. Use `optics_ui_damage_rects` for the set, and this only at the point
 * where a single-rect API forces the collapse. */
static inline optics_gfx_rect optics_gfx_rect_union(optics_gfx_rect a, optics_gfx_rect b) {
    if (a.w <= 0.0f || a.h <= 0.0f)
        return b;
    if (b.w <= 0.0f || b.h <= 0.0f)
        return a;
    float x0 = a.x < b.x ? a.x : b.x;
    float y0 = a.y < b.y ? a.y : b.y;
    float ax1 = a.x + a.w, bx1 = b.x + b.w;
    float ay1 = a.y + a.h, by1 = b.y + b.h;
    float x1 = ax1 > bx1 ? ax1 : bx1;
    float y1 = ay1 > by1 ? ay1 : by1;
    return (optics_gfx_rect){x0, y0, x1 - x0, y1 - y0};
}

/* ================================================================== */
/*  2D affine matrix (3 rows × 2 cols)                                */
/* ================================================================== */

typedef struct optics_gfx_mat3x2 {
    float m[6];
} optics_gfx_mat3x2;

OPTICS_GFX_API optics_gfx_mat3x2 optics_gfx_mat3x2_identity(void);
OPTICS_GFX_API optics_gfx_mat3x2 optics_gfx_mat3x2_translate(float x, float y);
OPTICS_GFX_API optics_gfx_mat3x2 optics_gfx_mat3x2_scale(float x, float y);
OPTICS_GFX_API optics_gfx_mat3x2 optics_gfx_mat3x2_rotate(float radians);
OPTICS_GFX_API optics_gfx_mat3x2 optics_gfx_mat3x2_multiply(optics_gfx_mat3x2 a,
                                                            optics_gfx_mat3x2 b);
OPTICS_GFX_API optics_gfx_mat3x2 optics_gfx_mat3x2_invert(optics_gfx_mat3x2 m);
OPTICS_GFX_API bool optics_gfx_mat3x2_is_identity(optics_gfx_mat3x2 m);
OPTICS_GFX_API optics_gfx_point optics_gfx_mat3x2_transform_point(optics_gfx_mat3x2 m,
                                                                  optics_gfx_point p);
OPTICS_GFX_API optics_gfx_rect optics_gfx_mat3x2_transform_rect(optics_gfx_mat3x2 m,
                                                                optics_gfx_rect r);

/* ================================================================== */
/*  Quaternion (defined before mat4 because mat4 references it)       */
/* ================================================================== */

typedef struct optics_gfx_quat {
    float x, y, z, w;
} optics_gfx_quat;

/* ================================================================== */
/*  4×4 matrix                                                        */
/* ================================================================== */

typedef struct optics_gfx_mat4 {
    float m[16];
} optics_gfx_mat4;

OPTICS_GFX_API optics_gfx_mat4 optics_gfx_mat4_identity(void);
OPTICS_GFX_API optics_gfx_mat4 optics_gfx_mat4_translate(float x, float y, float z);
OPTICS_GFX_API optics_gfx_mat4 optics_gfx_mat4_scale(float x, float y, float z);
OPTICS_GFX_API optics_gfx_mat4 optics_gfx_mat4_rotation_quat(optics_gfx_quat q);
OPTICS_GFX_API optics_gfx_mat4 optics_gfx_mat4_multiply(optics_gfx_mat4 a, optics_gfx_mat4 b);
OPTICS_GFX_API optics_gfx_vec4 optics_gfx_mat4_transform_vec4(optics_gfx_mat4 m, optics_gfx_vec4 v);
OPTICS_GFX_API optics_gfx_mat4 optics_gfx_mat4_invert(optics_gfx_mat4 m);
OPTICS_GFX_API optics_gfx_mat4 optics_gfx_mat4_perspective(float fov_y_rad, float aspect,
                                                           float z_near, float z_far);
OPTICS_GFX_API optics_gfx_mat4 optics_gfx_mat4_orthographic(float left, float right, float bottom,
                                                            float top, float z_near, float z_far);
OPTICS_GFX_API optics_gfx_mat4 optics_gfx_mat4_look_at(optics_gfx_vec3 eye, optics_gfx_vec3 center,
                                                       optics_gfx_vec3 up);

/* ================================================================== */
/*  3×3 matrix (color-space transforms; column-major like optics_gfx_mat4)  */
/* ================================================================== */

typedef struct optics_gfx_mat3 {
    float m[9]; /* column-major: m[col*3 + row] */
} optics_gfx_mat3;

OPTICS_GFX_API optics_gfx_mat3 optics_gfx_mat3_identity(void);
OPTICS_GFX_API optics_gfx_mat3
optics_gfx_mat3_multiply(optics_gfx_mat3 a, optics_gfx_mat3 b); /* a·b applies b first */
OPTICS_GFX_API optics_gfx_vec3 optics_gfx_mat3_transform_vec3(optics_gfx_mat3 m, optics_gfx_vec3 v);
OPTICS_GFX_API optics_gfx_mat3
optics_gfx_mat3_invert(optics_gfx_mat3 m); /* identity when singular */

/* ================================================================== */
/*  Quaternion operations                                             */
/* ================================================================== */

OPTICS_GFX_API optics_gfx_quat optics_gfx_quat_identity(void);
OPTICS_GFX_API optics_gfx_quat optics_gfx_quat_axis_angle(optics_gfx_vec3 axis, float radians);
OPTICS_GFX_API optics_gfx_quat optics_gfx_quat_multiply(optics_gfx_quat a, optics_gfx_quat b);
OPTICS_GFX_API optics_gfx_quat optics_gfx_quat_normalize(optics_gfx_quat q);
OPTICS_GFX_API optics_gfx_vec3 optics_gfx_quat_rotate(optics_gfx_quat q, optics_gfx_vec3 v);
OPTICS_GFX_API optics_gfx_quat optics_gfx_quat_slerp(optics_gfx_quat a, optics_gfx_quat b, float t);

/* ================================================================== */
/*  Colour — packed AARRGGBB premultiplied, 8-bit per channel         */
/* ================================================================== */

typedef uint32_t optics_gfx_color;

OPTICS_GFX_API optics_gfx_color optics_gfx_color_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
OPTICS_GFX_API optics_gfx_color optics_gfx_color_rgba_premul(uint8_t r, uint8_t g, uint8_t b,
                                                             uint8_t a);
OPTICS_GFX_API void optics_gfx_color_unpack(optics_gfx_color c, uint8_t *r, uint8_t *g, uint8_t *b,
                                            uint8_t *a);
OPTICS_GFX_API optics_gfx_vec4 optics_gfx_color_to_linear(optics_gfx_color c);
OPTICS_GFX_API optics_gfx_color optics_gfx_color_from_linear(optics_gfx_vec4 linear);

/* ================================================================== */
/*  Color-space math (ADR-0069)                                       */
/* ================================================================== */

/* Validation and equality for optics_gfx_color_space values. Custom spaces are
 * valid only with sane chromaticities (xy in (0,1), non-degenerate
 * triangle, non-zero white); OPTICS_GFX_TRANSFER_GAMMA requires gamma > 0. */
OPTICS_GFX_API bool optics_gfx_color_space_is_valid(optics_gfx_color_space cs);
OPTICS_GFX_API bool optics_gfx_color_space_equal(optics_gfx_color_space a,
                                                 optics_gfx_color_space b);

/* Scalar transfer functions between linear light and the encoded
 * domain. `gamma` is read only for OPTICS_GFX_TRANSFER_GAMMA. PQ linear is in
 * units of 10000 cd/m²; HLG is nominal 0..1 scene light. Encode clamps
 * to the representable range; decode mirrors it. */
OPTICS_GFX_API float optics_gfx_transfer_encode(optics_gfx_transfer_func tf, float gamma,
                                                float linear);
OPTICS_GFX_API float optics_gfx_transfer_decode(optics_gfx_transfer_func tf, float gamma,
                                                float encoded);

/* Builds the 3×3 matrix converting linear-light RGB in `from` to
 * linear-light RGB in `to` (primaries only — transfer functions are
 * applied separately). Bradford-adapts when the white points differ.
 * Returns false (and identity) when either space is invalid. */
OPTICS_GFX_API bool optics_gfx_color_space_transform_matrix(optics_gfx_color_space from,
                                                            optics_gfx_color_space to,
                                                            optics_gfx_mat3 *out);

/* ================================================================== */
/*  Arena allocator (CPU-side bump)                                   */
/* ================================================================== */

typedef struct optics_gfx_arena {
    uint8_t *base;
    size_t capacity;
    size_t used;
    optics_gfx_allocator alloc;
    bool owns_buffer;
} optics_gfx_arena;

OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result optics_gfx_arena_init(
    optics_gfx_arena *a, size_t capacity, const optics_gfx_allocator *alloc /* nullable */);

OPTICS_GFX_API void optics_gfx_arena_deinit(optics_gfx_arena *a);
OPTICS_GFX_API void *optics_gfx_arena_alloc(optics_gfx_arena *a, size_t bytes);
OPTICS_GFX_API void *optics_gfx_arena_alloc_aligned(optics_gfx_arena *a, size_t bytes,
                                                    size_t align);
OPTICS_GFX_API void optics_gfx_arena_reset(optics_gfx_arena *a);

/* Bucket an extent value to the nearest multiple of `step` (e.g. 128px),
 * preventing dynamic size sweeps from thrashing render-target allocations. */
static inline uint32_t optics_gfx_bucket_extent(uint32_t extent, uint32_t step) {
    if (extent == 0 || step == 0)
        return 0;
    return ((extent + step - 1) / step) * step;
}

#ifdef __cplusplus
}
#endif

#endif /* OPTICS_GFX_MATH_H */
