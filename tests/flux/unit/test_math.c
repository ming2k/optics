/*
 * Stage 3 math tests. Cover the load-bearing properties:
 *   - identity: id * x == x, x * id == x
 *   - multiply: linearity + associativity sample
 *   - perspective: forward/back z mapping into [0, 1]
 *   - look_at: eye+forward -> camera-space origin on -Z
 *   - invert: m * m^-1 == identity (within eps)
 *   - quat round-trip: axis-angle, normalize, rotate
 *   - color: pack -> unpack round-trip; premul math
 *   - mat3x2: identity, translate, scale, rotate composition
 */
#include "test_helpers.h"
#include <flux/flux.h>

#define EPS 1e-4f

static bool mat4_near(flux_mat4 a, flux_mat4 b, float eps) {
    for (int i = 0; i < 16; ++i) {
        float d = a.m[i] - b.m[i];
        if (d < 0)
            d = -d;
        if (d > eps)
            return false;
    }
    return true;
}

int main(void) {
    /* --- vec3 basics --- */
    {
        flux_vec3 a = flux_vec3_make(1, 2, 2);
        EXPECT_NEAR(flux_vec3_length(a), 3.0f, EPS);
        flux_vec3 n = flux_vec3_normalize(a);
        EXPECT_NEAR(flux_vec3_length(n), 1.0f, EPS);

        flux_vec3 x = {1, 0, 0};
        flux_vec3 y = {0, 1, 0};
        flux_vec3 z = flux_vec3_cross(x, y);
        EXPECT_NEAR(z.x, 0.0f, EPS);
        EXPECT_NEAR(z.y, 0.0f, EPS);
        EXPECT_NEAR(z.z, 1.0f, EPS);
    }

    /* --- mat3x2 --- */
    {
        EXPECT(flux_mat3x2_is_identity(flux_mat3x2_identity()));

        flux_mat3x2 t = flux_mat3x2_translate(10.0f, 20.0f);
        flux_point p = flux_mat3x2_transform_point(t, (flux_point){1.0f, 2.0f});
        EXPECT_NEAR(p.x, 11.0f, EPS);
        EXPECT_NEAR(p.y, 22.0f, EPS);

        flux_mat3x2 s = flux_mat3x2_scale(2.0f, 3.0f);
        flux_mat3x2 ts = flux_mat3x2_multiply(t, s);
        flux_point p2 = flux_mat3x2_transform_point(ts, (flux_point){1.0f, 2.0f});
        /* (1*2, 2*3) then translate (10, 20) -> (12, 26) */
        EXPECT_NEAR(p2.x, 12.0f, EPS);
        EXPECT_NEAR(p2.y, 26.0f, EPS);

        flux_mat3x2 i = flux_mat3x2_invert(t);
        flux_mat3x2 r = flux_mat3x2_multiply(t, i);
        EXPECT(flux_mat3x2_is_identity(r));
    }

    /* --- mat3x2 transform_rect AABB --- */
    {
        /* 90deg rotation should expand a unit square's AABB symmetrically. */
        flux_mat3x2 r = flux_mat3x2_rotate(1.57079632679f); /* ~pi/2 */
        flux_rect aabb = flux_mat3x2_transform_rect(r, (flux_rect){0, 0, 1, 1});
        EXPECT_NEAR(aabb.w, 1.0f, EPS);
        EXPECT_NEAR(aabb.h, 1.0f, EPS);
    }

    /* --- mat4 identity / multiply --- */
    {
        flux_mat4 id = flux_mat4_identity();
        flux_mat4 t = flux_mat4_translate(5, 6, 7);
        flux_mat4 r1 = flux_mat4_multiply(t, id);
        flux_mat4 r2 = flux_mat4_multiply(id, t);
        EXPECT(mat4_near(r1, t, EPS));
        EXPECT(mat4_near(r2, t, EPS));
    }

    /* --- mat4 invert --- */
    {
        flux_mat4 t = flux_mat4_translate(1, 2, 3);
        flux_mat4 ti = flux_mat4_invert(t);
        flux_mat4 r = flux_mat4_multiply(t, ti);
        EXPECT(mat4_near(r, flux_mat4_identity(), EPS));

        flux_mat4 s = flux_mat4_scale(2, 4, 8);
        flux_mat4 si = flux_mat4_invert(s);
        flux_mat4 r2 = flux_mat4_multiply(s, si);
        EXPECT(mat4_near(r2, flux_mat4_identity(), EPS));
    }

    /* --- mat4 perspective: z=near -> NDC z=0, z=far -> NDC z=1 --- */
    {
        flux_mat4 p = flux_mat4_perspective(1.0f, 1.0f, 0.5f, 100.0f);
        /* Point at z = -0.5 (= -near in view space; camera looks -Z) */
        flux_vec4 near = flux_mat4_transform_vec4(p, (flux_vec4){0, 0, -0.5f, 1});
        float ndc_z_near = near.z / near.w;
        EXPECT_NEAR(ndc_z_near, 0.0f, EPS);

        flux_vec4 far = flux_mat4_transform_vec4(p, (flux_vec4){0, 0, -100.0f, 1});
        float ndc_z_far = far.z / far.w;
        EXPECT_NEAR(ndc_z_far, 1.0f, EPS);
    }

    /* --- mat4 look_at: eye - center should map to camera +Z (we look -Z) --- */
    {
        flux_vec3 eye = {0, 0, 5};
        flux_vec3 center = {0, 0, 0};
        flux_vec3 up = {0, 1, 0};
        flux_mat4 v = flux_mat4_look_at(eye, center, up);

        /* The eye position transformed to view space should be origin. */
        flux_vec4 ep = flux_mat4_transform_vec4(v, (flux_vec4){eye.x, eye.y, eye.z, 1});
        EXPECT_NEAR(ep.x, 0.0f, EPS);
        EXPECT_NEAR(ep.y, 0.0f, EPS);
        EXPECT_NEAR(ep.z, 0.0f, EPS);
    }

    /* --- quat axis-angle round-trip via rotation --- */
    {
        /* 90deg around +Z should rotate (1,0,0) -> (0,1,0). */
        flux_quat q = flux_quat_axis_angle((flux_vec3){0, 0, 1}, 1.57079632679f);
        flux_vec3 r = flux_quat_rotate(q, (flux_vec3){1, 0, 0});
        EXPECT_NEAR(r.x, 0.0f, EPS);
        EXPECT_NEAR(r.y, 1.0f, EPS);
        EXPECT_NEAR(r.z, 0.0f, EPS);
    }

    /* --- quat identity --- */
    {
        flux_quat id = flux_quat_identity();
        flux_quat q = flux_quat_axis_angle((flux_vec3){0, 1, 0}, 0.5f);
        flux_quat qid = flux_quat_multiply(q, id);
        EXPECT_NEAR(qid.x, q.x, EPS);
        EXPECT_NEAR(qid.y, q.y, EPS);
        EXPECT_NEAR(qid.z, q.z, EPS);
        EXPECT_NEAR(qid.w, q.w, EPS);
    }

    /* --- quat slerp endpoints --- */
    {
        flux_quat a = flux_quat_identity();
        flux_quat b = flux_quat_axis_angle((flux_vec3){0, 1, 0}, 1.0f);
        flux_quat at0 = flux_quat_slerp(a, b, 0.0f);
        flux_quat at1 = flux_quat_slerp(a, b, 1.0f);
        EXPECT_NEAR(at0.w, a.w, EPS);
        EXPECT_NEAR(at1.w, b.w, EPS);
    }

    /* --- mat4_rotation_quat round-trip --- */
    {
        flux_quat q = flux_quat_axis_angle((flux_vec3){0, 0, 1}, 1.57079632679f);
        flux_mat4 m = flux_mat4_rotation_quat(q);
        flux_vec4 r = flux_mat4_transform_vec4(m, (flux_vec4){1, 0, 0, 0});
        EXPECT_NEAR(r.x, 0.0f, EPS);
        EXPECT_NEAR(r.y, 1.0f, EPS);
        EXPECT_NEAR(r.z, 0.0f, EPS);
    }

    /* --- color pack / unpack round-trip --- */
    {
        flux_color c = flux_color_rgba(0x12, 0x34, 0x56, 0x78);
        uint8_t r, g, b, a;
        flux_color_unpack(c, &r, &g, &b, &a);
        EXPECT(r == 0x12);
        EXPECT(g == 0x34);
        EXPECT(b == 0x56);
        EXPECT(a == 0x78);
    }

    /* --- color premul: 50% alpha halves channels --- */
    {
        flux_color c = flux_color_rgba_premul(200, 100, 50, 128);
        uint8_t r, g, b, a;
        flux_color_unpack(c, &r, &g, &b, &a);
        EXPECT(a == 128);
        EXPECT(r >= 99 && r <= 101); /* 200 * 128 / 255 ~= 100 */
        EXPECT(g >= 49 && g <= 51);
        EXPECT(b >= 24 && b <= 26);
    }

    /* --- color round-trip through linear (opaque) --- */
    {
        flux_color c1 = flux_color_rgba_premul(180, 90, 45, 255);
        flux_vec4 lin = flux_color_to_linear(c1);
        flux_color c2 = flux_color_from_linear(lin);
        uint8_t r1, g1, b1, a1, r2, g2, b2, a2;
        flux_color_unpack(c1, &r1, &g1, &b1, &a1);
        flux_color_unpack(c2, &r2, &g2, &b2, &a2);
        /* Allow ±1 in each channel from sRGB rounding. */
        int dr = (int)r2 - (int)r1;
        if (dr < 0)
            dr = -dr;
        int dg = (int)g2 - (int)g1;
        if (dg < 0)
            dg = -dg;
        int db = (int)b2 - (int)b1;
        if (db < 0)
            db = -db;
        EXPECT(dr <= 1);
        EXPECT(dg <= 1);
        EXPECT(db <= 1);
        EXPECT(a2 == 255);
    }

    /* --- arena alignment --- */
    {
        flux_arena arena;
        EXPECT(flux_arena_init(&arena, 1024, nullptr) == FLUX_OK);
        char *one = flux_arena_alloc(&arena, 1);
        EXPECT(one != nullptr);
        void *al = flux_arena_alloc_aligned(&arena, 16, 64);
        EXPECT(al != nullptr);
        EXPECT(((uintptr_t)al & 63u) == 0);
        flux_arena_destroy(&arena);
    }

    /* --- arena: zero-size init rejected; OOM after exhaustion --- */
    {
        flux_arena a;
        EXPECT(flux_arena_init(&a, 0, nullptr) == FLUX_ERROR_INVALID_ARGUMENT);

        EXPECT(flux_arena_init(&a, 256, nullptr) == FLUX_OK);
        EXPECT(flux_arena_alloc(&a, 1024) == nullptr); /* immediate OOM */
        flux_arena_reset(&a);
        EXPECT(flux_arena_alloc(&a, 200) != nullptr);
        flux_arena_destroy(&a);
    }

    /* --- vec4 ops (gaps in original smoke) --- */
    {
        flux_vec4 a = flux_vec4_make(1, 2, 3, 4);
        flux_vec4 b = flux_vec4_make(5, 6, 7, 8);
        flux_vec4 s = flux_vec4_add(a, b);
        EXPECT(s.x == 6.0f && s.y == 8.0f && s.z == 10.0f && s.w == 12.0f);

        flux_vec4 k = flux_vec4_scale(a, 0.5f);
        EXPECT(k.x == 0.5f && k.w == 2.0f);

        EXPECT(flux_vec4_dot(a, b) == (1.0f * 5 + 2.0f * 6 + 3.0f * 7 + 4.0f * 8));
    }

    /* --- color: premul with a=255 is identity --- */
    {
        flux_color c = flux_color_rgba_premul(0x12, 0x34, 0x56, 0xFF);
        uint8_t r, g, b, a;
        flux_color_unpack(c, &r, &g, &b, &a);
        EXPECT(r == 0x12 && g == 0x34 && b == 0x56 && a == 0xFF);
    }

    /* --- color: premul with a=0 zeros the channels --- */
    {
        flux_color c = flux_color_rgba_premul(0xFF, 0xFF, 0xFF, 0);
        EXPECT(c == 0);
    }

    TEST_SUMMARY();
}
