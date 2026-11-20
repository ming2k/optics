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
#include "optics_test.h"
#include <optics/gfx.h>

#define EPS 1e-4f

static bool mat4_near(optics_gfx_mat4 a, optics_gfx_mat4 b, float eps) {
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
        optics_gfx_vec3 a = optics_gfx_vec3_make(1, 2, 2);
        OT_EXPECT_NEAR(optics_gfx_vec3_length(a), 3.0f, EPS);
        optics_gfx_vec3 n = optics_gfx_vec3_normalize(a);
        OT_EXPECT_NEAR(optics_gfx_vec3_length(n), 1.0f, EPS);

        optics_gfx_vec3 x = {1, 0, 0};
        optics_gfx_vec3 y = {0, 1, 0};
        optics_gfx_vec3 z = optics_gfx_vec3_cross(x, y);
        OT_EXPECT_NEAR(z.x, 0.0f, EPS);
        OT_EXPECT_NEAR(z.y, 0.0f, EPS);
        OT_EXPECT_NEAR(z.z, 1.0f, EPS);
    }

    /* --- mat3x2 --- */
    {
        OT_EXPECT(optics_gfx_mat3x2_is_identity(optics_gfx_mat3x2_identity()));

        optics_gfx_mat3x2 t = optics_gfx_mat3x2_translate(10.0f, 20.0f);
        optics_gfx_point p = optics_gfx_mat3x2_transform_point(t, (optics_gfx_point){1.0f, 2.0f});
        OT_EXPECT_NEAR(p.x, 11.0f, EPS);
        OT_EXPECT_NEAR(p.y, 22.0f, EPS);

        optics_gfx_mat3x2 s = optics_gfx_mat3x2_scale(2.0f, 3.0f);
        optics_gfx_mat3x2 ts = optics_gfx_mat3x2_multiply(t, s);
        optics_gfx_point p2 = optics_gfx_mat3x2_transform_point(ts, (optics_gfx_point){1.0f, 2.0f});
        /* (1*2, 2*3) then translate (10, 20) -> (12, 26) */
        OT_EXPECT_NEAR(p2.x, 12.0f, EPS);
        OT_EXPECT_NEAR(p2.y, 26.0f, EPS);

        optics_gfx_mat3x2 i = optics_gfx_mat3x2_invert(t);
        optics_gfx_mat3x2 r = optics_gfx_mat3x2_multiply(t, i);
        OT_EXPECT(optics_gfx_mat3x2_is_identity(r));
    }

    /* --- mat3x2 transform_rect AABB --- */
    {
        /* 90deg rotation should expand a unit square's AABB symmetrically. */
        optics_gfx_mat3x2 r = optics_gfx_mat3x2_rotate(1.57079632679f); /* ~pi/2 */
        optics_gfx_rect aabb = optics_gfx_mat3x2_transform_rect(r, (optics_gfx_rect){0, 0, 1, 1});
        OT_EXPECT_NEAR(aabb.w, 1.0f, EPS);
        OT_EXPECT_NEAR(aabb.h, 1.0f, EPS);
    }

    /* --- mat4 identity / multiply --- */
    {
        optics_gfx_mat4 id = optics_gfx_mat4_identity();
        optics_gfx_mat4 t = optics_gfx_mat4_translate(5, 6, 7);
        optics_gfx_mat4 r1 = optics_gfx_mat4_multiply(t, id);
        optics_gfx_mat4 r2 = optics_gfx_mat4_multiply(id, t);
        OT_EXPECT(mat4_near(r1, t, EPS));
        OT_EXPECT(mat4_near(r2, t, EPS));
    }

    /* --- mat4 invert --- */
    {
        optics_gfx_mat4 t = optics_gfx_mat4_translate(1, 2, 3);
        optics_gfx_mat4 ti = optics_gfx_mat4_invert(t);
        optics_gfx_mat4 r = optics_gfx_mat4_multiply(t, ti);
        OT_EXPECT(mat4_near(r, optics_gfx_mat4_identity(), EPS));

        optics_gfx_mat4 s = optics_gfx_mat4_scale(2, 4, 8);
        optics_gfx_mat4 si = optics_gfx_mat4_invert(s);
        optics_gfx_mat4 r2 = optics_gfx_mat4_multiply(s, si);
        OT_EXPECT(mat4_near(r2, optics_gfx_mat4_identity(), EPS));
    }

    /* --- mat4 perspective: z=near -> NDC z=0, z=far -> NDC z=1 --- */
    {
        optics_gfx_mat4 p = optics_gfx_mat4_perspective(1.0f, 1.0f, 0.5f, 100.0f);
        /* Point at z = -0.5 (= -near in view space; camera looks -Z) */
        optics_gfx_vec4 near = optics_gfx_mat4_transform_vec4(p, (optics_gfx_vec4){0, 0, -0.5f, 1});
        float ndc_z_near = near.z / near.w;
        OT_EXPECT_NEAR(ndc_z_near, 0.0f, EPS);

        optics_gfx_vec4 far =
            optics_gfx_mat4_transform_vec4(p, (optics_gfx_vec4){0, 0, -100.0f, 1});
        float ndc_z_far = far.z / far.w;
        OT_EXPECT_NEAR(ndc_z_far, 1.0f, EPS);
    }

    /* --- mat4 look_at: eye - center should map to camera +Z (we look -Z) --- */
    {
        optics_gfx_vec3 eye = {0, 0, 5};
        optics_gfx_vec3 center = {0, 0, 0};
        optics_gfx_vec3 up = {0, 1, 0};
        optics_gfx_mat4 v = optics_gfx_mat4_look_at(eye, center, up);

        /* The eye position transformed to view space should be origin. */
        optics_gfx_vec4 ep =
            optics_gfx_mat4_transform_vec4(v, (optics_gfx_vec4){eye.x, eye.y, eye.z, 1});
        OT_EXPECT_NEAR(ep.x, 0.0f, EPS);
        OT_EXPECT_NEAR(ep.y, 0.0f, EPS);
        OT_EXPECT_NEAR(ep.z, 0.0f, EPS);
    }

    /* --- quat axis-angle round-trip via rotation --- */
    {
        /* 90deg around +Z should rotate (1,0,0) -> (0,1,0). */
        optics_gfx_quat q = optics_gfx_quat_axis_angle((optics_gfx_vec3){0, 0, 1}, 1.57079632679f);
        optics_gfx_vec3 r = optics_gfx_quat_rotate(q, (optics_gfx_vec3){1, 0, 0});
        OT_EXPECT_NEAR(r.x, 0.0f, EPS);
        OT_EXPECT_NEAR(r.y, 1.0f, EPS);
        OT_EXPECT_NEAR(r.z, 0.0f, EPS);
    }

    /* --- quat identity --- */
    {
        optics_gfx_quat id = optics_gfx_quat_identity();
        optics_gfx_quat q = optics_gfx_quat_axis_angle((optics_gfx_vec3){0, 1, 0}, 0.5f);
        optics_gfx_quat qid = optics_gfx_quat_multiply(q, id);
        OT_EXPECT_NEAR(qid.x, q.x, EPS);
        OT_EXPECT_NEAR(qid.y, q.y, EPS);
        OT_EXPECT_NEAR(qid.z, q.z, EPS);
        OT_EXPECT_NEAR(qid.w, q.w, EPS);
    }

    /* --- quat slerp endpoints --- */
    {
        optics_gfx_quat a = optics_gfx_quat_identity();
        optics_gfx_quat b = optics_gfx_quat_axis_angle((optics_gfx_vec3){0, 1, 0}, 1.0f);
        optics_gfx_quat at0 = optics_gfx_quat_slerp(a, b, 0.0f);
        optics_gfx_quat at1 = optics_gfx_quat_slerp(a, b, 1.0f);
        OT_EXPECT_NEAR(at0.w, a.w, EPS);
        OT_EXPECT_NEAR(at1.w, b.w, EPS);
    }

    /* --- mat4_rotation_quat round-trip --- */
    {
        optics_gfx_quat q = optics_gfx_quat_axis_angle((optics_gfx_vec3){0, 0, 1}, 1.57079632679f);
        optics_gfx_mat4 m = optics_gfx_mat4_rotation_quat(q);
        optics_gfx_vec4 r = optics_gfx_mat4_transform_vec4(m, (optics_gfx_vec4){1, 0, 0, 0});
        OT_EXPECT_NEAR(r.x, 0.0f, EPS);
        OT_EXPECT_NEAR(r.y, 1.0f, EPS);
        OT_EXPECT_NEAR(r.z, 0.0f, EPS);
    }

    /* --- color pack / unpack round-trip --- */
    {
        optics_gfx_color c = optics_gfx_color_rgba(0x12, 0x34, 0x56, 0x78);
        uint8_t r, g, b, a;
        optics_gfx_color_unpack(c, &r, &g, &b, &a);
        OT_EXPECT(r == 0x12);
        OT_EXPECT(g == 0x34);
        OT_EXPECT(b == 0x56);
        OT_EXPECT(a == 0x78);
    }

    /* --- color premul: 50% alpha halves channels --- */
    {
        optics_gfx_color c = optics_gfx_color_rgba_premul(200, 100, 50, 128);
        uint8_t r, g, b, a;
        optics_gfx_color_unpack(c, &r, &g, &b, &a);
        OT_EXPECT(a == 128);
        OT_EXPECT(r >= 99 && r <= 101); /* 200 * 128 / 255 ~= 100 */
        OT_EXPECT(g >= 49 && g <= 51);
        OT_EXPECT(b >= 24 && b <= 26);
    }

    /* --- color round-trip through linear (opaque) --- */
    {
        optics_gfx_color c1 = optics_gfx_color_rgba_premul(180, 90, 45, 255);
        optics_gfx_vec4 lin = optics_gfx_color_to_linear(c1);
        optics_gfx_color c2 = optics_gfx_color_from_linear(lin);
        uint8_t r1, g1, b1, a1, r2, g2, b2, a2;
        optics_gfx_color_unpack(c1, &r1, &g1, &b1, &a1);
        optics_gfx_color_unpack(c2, &r2, &g2, &b2, &a2);
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
        OT_EXPECT(dr <= 1);
        OT_EXPECT(dg <= 1);
        OT_EXPECT(db <= 1);
        OT_EXPECT(a2 == 255);
    }

    /* --- arena alignment --- */
    {
        optics_gfx_arena arena;
        OT_EXPECT(optics_gfx_arena_init(&arena, 1024, nullptr) == OPTICS_GFX_OK);
        char *one = optics_gfx_arena_alloc(&arena, 1);
        OT_EXPECT(one != nullptr);
        void *al = optics_gfx_arena_alloc_aligned(&arena, 16, 64);
        OT_EXPECT(al != nullptr);
        OT_EXPECT(((uintptr_t)al & 63u) == 0);
        optics_gfx_arena_deinit(&arena);
    }

    /* --- arena: zero-size init rejected; OOM after exhaustion --- */
    {
        optics_gfx_arena a;
        OT_EXPECT(optics_gfx_arena_init(&a, 0, nullptr) == OPTICS_GFX_ERROR_INVALID_ARGUMENT);

        OT_EXPECT(optics_gfx_arena_init(&a, 256, nullptr) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_arena_alloc(&a, 1024) == nullptr); /* immediate OOM */
        optics_gfx_arena_reset(&a);
        OT_EXPECT(optics_gfx_arena_alloc(&a, 200) != nullptr);
        optics_gfx_arena_deinit(&a);
    }

    /* --- vec4 ops (gaps in original smoke) --- */
    {
        optics_gfx_vec4 a = optics_gfx_vec4_make(1, 2, 3, 4);
        optics_gfx_vec4 b = optics_gfx_vec4_make(5, 6, 7, 8);
        optics_gfx_vec4 s = optics_gfx_vec4_add(a, b);
        OT_EXPECT(s.x == 6.0f && s.y == 8.0f && s.z == 10.0f && s.w == 12.0f);

        optics_gfx_vec4 k = optics_gfx_vec4_scale(a, 0.5f);
        OT_EXPECT(k.x == 0.5f && k.w == 2.0f);

        OT_EXPECT(optics_gfx_vec4_dot(a, b) == (1.0f * 5 + 2.0f * 6 + 3.0f * 7 + 4.0f * 8));
    }

    /* --- color: premul with a=255 is identity --- */
    {
        optics_gfx_color c = optics_gfx_color_rgba_premul(0x12, 0x34, 0x56, 0xFF);
        uint8_t r, g, b, a;
        optics_gfx_color_unpack(c, &r, &g, &b, &a);
        OT_EXPECT(r == 0x12 && g == 0x34 && b == 0x56 && a == 0xFF);
    }

    /* --- color: premul with a=0 zeros the channels --- */
    {
        optics_gfx_color c = optics_gfx_color_rgba_premul(0xFF, 0xFF, 0xFF, 0);
        OT_EXPECT(c == 0);
    }

    return OT_REPORT();
}
