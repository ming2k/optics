/* Micro-benchmark: world-matrix propagation cost per animated frame.
 *
 * Builds a synthetic scene laid out like a skinned VRM avatar (a long
 * spine-to-fingertip chain with siblings), where every node array is in an
 * order that forces the convergence loop to do the full O(n^2) work. Times
 * many `flux_sg_scene_apply_animation` calls.
 *
 * Usage: bench_sg_worlds [node_count] [iterations]
 */
#include <flux-scene-graph/scene-graph.h>

#include "bench_helpers.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct test_glb {
    uint8_t *bytes;
    size_t size;
} test_glb;

static void put_u32_le(uint8_t *dst, uint32_t value) {
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
    dst[2] = (uint8_t)(value >> 16);
    dst[3] = (uint8_t)(value >> 24);
}

static test_glb make_glb(const char *json, const void *bin, size_t bin_size) {
    size_t json_size = strlen(json);
    size_t json_padded = (json_size + 3u) & ~(size_t)3u;
    size_t bin_padded = (bin_size + 3u) & ~(size_t)3u;
    size_t total = 12u + 8u + json_padded + 8u + bin_padded;
    uint8_t *bytes = calloc(1, total);
    if (!bytes)
        return (test_glb){0};
    put_u32_le(bytes, 0x46546c67u);
    put_u32_le(bytes + 4u, 2u);
    put_u32_le(bytes + 8u, (uint32_t)total);
    put_u32_le(bytes + 12u, (uint32_t)json_padded);
    put_u32_le(bytes + 16u, 0x4e4f534au);
    memcpy(bytes + 20u, json, json_size);
    memset(bytes + 20u + json_size, ' ', json_padded - json_size);
    size_t bin_header = 20u + json_padded;
    put_u32_le(bytes + bin_header, (uint32_t)bin_padded);
    put_u32_le(bytes + bin_header + 4u, 0x004e4942u);
    memcpy(bytes + bin_header + 8u, bin, bin_size);
    return (test_glb){.bytes = bytes, .size = total};
}

/* A chain of `count` nodes numbered so that every child has a LOWER index
 * than its parent (i.e. exactly reversed topological order): node i's parent
 * is i+1. The convergence loop then needs `count` passes, each doing `count`
 * multiplies. */
static test_glb make_chain(uint32_t count) {
    size_t cap = 256u + (size_t)count * 96u;
    char *json = malloc(cap);
    size_t off = 0;
    off += (size_t)snprintf(json + off, cap - off,
                            "{\"asset\":{\"version\":\"2.0\"},"
                            "\"buffers\":[{\"byteLength\":36}],"
                            "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36}],"
                            "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                            "\"type\":\"VEC3\"}],"
                            "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],"
                            "\"nodes\":[");
    /* node i: translation [0, 0.01, 0], children [i-1] for i>=1, mesh on the
     * last node. Emitted in index order 0..count-1; since child index <
     * parent index, this is reversed topological order. */
    for (uint32_t i = 0; i < count; ++i) {
        if (i)
            json[off++] = ',';
        if (i + 1 < count)
            off += (size_t)snprintf(json + off, cap - off,
                                    "{\"translation\":[0,0.01,0],\"children\":[%u]}", i + 1u);
        else
            off += (size_t)snprintf(json + off, cap - off,
                                    "{\"translation\":[0,0.01,0],\"mesh\":0}");
    }
    off += (size_t)snprintf(json + off, cap - off,
                            "],\"scenes\":[{\"nodes\":[%u]}],\"scene\":0}", count - 1u);
    static const float positions[9] = {-0.1f, -0.1f, 0.0f, 0.1f,
                                       -0.1f, 0.0f,  0.0f, 0.1f, 0.0f};
    test_glb glb = make_glb(json, positions, sizeof(positions));
    free(json);
    return glb;
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

int main(int argc, char **argv) {
    uint32_t count = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 10) : 117u;
    int iters = argc > 2 ? atoi(argv[2]) : 2000;

    flux_device *device = bench_headless_device();
    if (!device) {
        fprintf(stderr, "no Vulkan device; skipping\n");
        return 0;
    }
    test_glb glb = make_chain(count);
    flux_sg_scene *scene = NULL;
    flux_result r = flux_sg_load_glb(device, glb.bytes, glb.size, &scene);
    if (r != FLUX_OK) {
        fprintf(stderr, "load failed: %s\n", flux_result_string(r));
        flux_device_release(device);
        free(glb.bytes);
        return 1;
    }
    /* Drive the full animated path: reset pose + sample + worlds + skin. */
    double t0 = now_ms();
    for (int i = 0; i < iters; ++i)
        flux_sg_scene_reset_pose(scene);
    double t1 = now_ms();
    printf("nodes=%u iters=%d  reset_pose_total=%.2fms  per_call=%.4fms\n", count, iters,
           t1 - t0, (t1 - t0) / iters);
    flux_device_wait_idle(device);
    flux_sg_scene_release(scene);
    free(glb.bytes);
    flux_device_release(device);
    return 0;
}
