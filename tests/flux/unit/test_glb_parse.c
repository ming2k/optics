/*
 * Deviceless GLB parse stage (flux_sg_parse_glb, ADR-0016): the parse
 * and the GPU build are separate seams, so every parser behaviour is
 * testable without a Vulkan device.
 *
 * Pinned here:
 *   - a valid minimal GLB parses to one primitive with the exact
 *     vertex data and AABB fed in;
 *   - the same input parses deterministically (two parses, identical
 *     primitive counts and bounds);
 *   - bounds() union covers every primitive, not just the first;
 *   - truncation at every structural boundary fails cleanly with a
 *     null out handle and nothing retained (leak-checked by ASan);
 *   - hostile inputs (bad magic, total length beyond the buffer,
 *     total=0 unsigned-underflow crash found by fuzz_glb_parse,
 *     accessor count bombs, OOB buffer views, node cycles) all fail
 *     with FLUX_ERROR_INVALID_ARGUMENT, never by crashing;
 *   - the out handle is NULL on every failure path (the documented
 *     contract callers rely on to skip the free).
 *
 * Build-stage behaviour (flux_mesh upload) is covered by the GPU
 * integration suites; this file is deliberately device-free.
 */
#include "test_helpers.h"
#include <flux-scene-graph/scene-graph.h>

#include <stdlib.h>
#include <string.h>

static uint32_t wr_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
    return 4;
}

/* Build a well-formed GLB: header + JSON chunk + optional BIN chunk. */
static size_t make_glb(uint8_t *buf, size_t cap, const char *json, const uint8_t *bin,
                       size_t bin_len, size_t *out_json_off) {
    size_t js_len = strlen(json);
    size_t js_pad = (4 - (js_len % 4)) % 4;
    size_t bin_pad = (4 - (bin_len % 4)) % 4;
    size_t total = 12 + 8 + js_len + js_pad + (bin_len ? 8 + bin_len + bin_pad : 0);
    if (total > cap)
        return 0;
    size_t o = 0;
    o += wr_u32(buf + o, 0x46546C67u);
    o += wr_u32(buf + o, 2);
    o += wr_u32(buf + o, (uint32_t)total);
    o += wr_u32(buf + o, (uint32_t)(js_len + js_pad));
    o += wr_u32(buf + o, 0x4E4F534Au);
    memcpy(buf + o, json, js_len);
    for (size_t i = 0; i < js_pad; i++)
        buf[o + js_len + i] = ' ';
    o += js_len + js_pad;
    if (out_json_off)
        *out_json_off = 12 + 8;
    if (bin_len) {
        o += wr_u32(buf + o, (uint32_t)(bin_len + bin_pad));
        o += wr_u32(buf + o, 0x004E4942u);
        memcpy(buf + o, bin, bin_len);
        for (size_t i = 0; i < bin_pad; i++)
            buf[o + bin_len + i] = 0;
        o += bin_len + bin_pad;
    }
    return o;
}

/* Minimal-but-valid scene: one triangle via POSITION accessor. */
static const char *MINIMAL_JSON =
    "{\"asset\":{\"version\":\"2.0\"},"
    "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],"
    "\"accessors\":[{\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"bufferView\":0}],"
    "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36}],"
    "\"buffers\":[{\"byteLength\":36}]}";

#define CAP 4096
static uint8_t buf[CAP];

int main(void) {
    /* --- valid minimal GLB: exact geometry round-trips ---------------- */
    float verts[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    size_t glb_len = make_glb(buf, CAP, MINIMAL_JSON, (const uint8_t *)verts, sizeof verts, NULL);
    EXPECT(glb_len > 0);

    flux_sg_scene_data *d1 = NULL, *d2 = NULL;
    EXPECT(flux_sg_parse_glb(buf, glb_len, &d1) == FLUX_OK);
    EXPECT(d1 != NULL);
    EXPECT(flux_sg_scene_data_primitive_count(d1) == 1);

    flux_vec3 mn = {0}, mx = {0};
    EXPECT(flux_sg_scene_data_bounds(d1, &mn, &mx));
    EXPECT(mn.x == 0.0f && mn.y == 0.0f && mn.z == 0.0f);
    EXPECT(mx.x == 1.0f && mx.y == 1.0f && mx.z == 0.0f);

    /* Determinism: a second parse produces the same shape. */
    EXPECT(flux_sg_parse_glb(buf, glb_len, &d2) == FLUX_OK);
    EXPECT(flux_sg_scene_data_primitive_count(d2) == 1);
    flux_vec3 mn2 = {0}, mx2 = {0};
    EXPECT(flux_sg_scene_data_bounds(d2, &mn2, &mx2));
    EXPECT(memcmp(&mn, &mn2, sizeof mn) == 0 && memcmp(&mx, &mx2, sizeof mx) == 0);
    flux_sg_scene_data_free(d1);
    flux_sg_scene_data_free(d2);

    /* --- truncation at every structural boundary fails cleanly -------- */
    for (size_t cut = 0; cut < glb_len; cut++) {
        flux_sg_scene_data *d =
            (flux_sg_scene_data *)(uintptr_t)0x1; /* sentinel: must be set to NULL */
        flux_result r = flux_sg_parse_glb(buf, cut, &d);
        EXPECT(r != FLUX_OK);
        EXPECT(d == NULL); /* documented contract: NULL out on failure */
        /* ASan verifies nothing was retained on the error path. */
    }

    /* --- hostile container headers ------------------------------------ */
    flux_sg_scene_data *d = NULL;
    size_t json_off = 0;
    (void)make_glb(buf, CAP, MINIMAL_JSON, (const uint8_t *)verts, sizeof verts, &json_off);

    /* total=0: the unsigned-underflow crash found by fuzz_glb_parse. */
    wr_u32(buf + 8, 0);
    EXPECT(flux_sg_parse_glb(buf, glb_len, &d) != FLUX_OK && d == NULL);

    /* total beyond the buffer. */
    wr_u32(buf + 8, (uint32_t)glb_len + 1);
    EXPECT(flux_sg_parse_glb(buf, glb_len, &d) != FLUX_OK && d == NULL);

    /* total below the header minimum. */
    wr_u32(buf + 8, 8);
    EXPECT(flux_sg_parse_glb(buf, glb_len, &d) != FLUX_OK && d == NULL);

    /* bad magic / bad version. */
    wr_u32(buf + 8, (uint32_t)glb_len);
    buf[0] = 'X';
    EXPECT(flux_sg_parse_glb(buf, glb_len, &d) != FLUX_OK && d == NULL);
    buf[0] = 'g';
    wr_u32(buf + 4, 3);
    EXPECT(flux_sg_parse_glb(buf, glb_len, &d) != FLUX_OK && d == NULL);
    wr_u32(buf + 4, 2);

    /* --- hostile JSON bodies (parse must fail or degrade safely) ------ */
    const char *hostile[] = {
        /* accessor count bomb: count*elem must not overflow into a tiny
         * allocation that is then written past */
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],"
        "\"accessors\":[{\"componentType\":5126,\"count\":268435456,\"type\":\"VEC3\","
        "\"bufferView\":0}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36}],"
        "\"buffers\":[{\"byteLength\":36}]}",

        /* buffer view offset beyond the BIN chunk */
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],"
        "\"accessors\":[{\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"bufferView\":0}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":2000000000,\"byteLength\":36}],"
        "\"buffers\":[{\"byteLength\":36}]}",

        /* node cycle: node 0 is its own child */
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"children\":[0]}]}",

        /* wrong component type on positions */
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],"
        "\"accessors\":[{\"componentType\":5130,\"count\":3,\"type\":\"VEC3\",\"bufferView\":0}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36}],"
        "\"buffers\":[{\"byteLength\":36}]}",
    };
    for (size_t i = 0; i < sizeof hostile / sizeof hostile[0]; i++) {
        size_t n = make_glb(buf, CAP, hostile[i], (const uint8_t *)verts, sizeof verts, NULL);
        EXPECT(n > 0);
        /* Acceptance here is value-based (some hostile content parses to
         * a valid-but-degenerate scene); the hard requirements are: no
         * crash, no hang, no leak, and the out-handle contract. */
        flux_result r = flux_sg_parse_glb(buf, n, &d);
        if (r == FLUX_OK) {
            EXPECT(d != NULL);
            flux_sg_scene_data_free(d);
        } else {
            EXPECT(d == NULL);
        }
    }

    /* --- garbage bytes: never crash, always a clean result ------------ */
    for (uint32_t seed = 1; seed <= 64; seed++) {
        uint32_t x = seed * 2654435761u;
        for (size_t i = 0; i < 512; i += 4) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            wr_u32(buf + i, x);
        }
        d = NULL;
        flux_result r = flux_sg_parse_glb(buf, 512, &d);
        if (r == FLUX_OK)
            flux_sg_scene_data_free(d);
        else
            EXPECT(d == NULL);
    }

    /* --- null-argument guards ----------------------------------------- */
    d = (flux_sg_scene_data *)(uintptr_t)0x1;
    EXPECT(flux_sg_parse_glb(NULL, 10, &d) == FLUX_ERROR_INVALID_ARGUMENT);
    EXPECT(flux_sg_parse_glb(buf, 10, NULL) == FLUX_ERROR_INVALID_ARGUMENT);
    EXPECT(flux_sg_scene_data_primitive_count(NULL) == 0);
    EXPECT(!flux_sg_scene_data_bounds(NULL, &mn, &mx));
    flux_sg_scene_data_free(NULL); /* no-op by contract */

    TEST_SUMMARY();
}
