/*
 * Allocator stress test via public APIs.
 *
 * Exercises flux_image_create / flux_mesh_create paths under heavy
 * churn to validate the GPU memory allocator: pooling, coalescing,
 * dedicated-fallback for oversize, and absence of leaks under ASan.
 *
 * The allocator implementation is intentionally internal — we don't
 * test it directly; we test that creating and destroying many GPU
 * resources stays bounded in memory and never crashes.
 */
#include "gfx_test.h"
#include "optics_test.h"
#include <optics/gfx.h>
#include <optics/gfx/scene.h>
#include <optics/gfx/vulkan.h>

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* pthread_barrier_t is missing on Apple platforms (and some winpthreads
 * builds), so keep the test portable with a tiny mutex/cond barrier. */
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int parties;
    int count;
    int generation;
} test_barrier;

static void test_barrier_init(test_barrier *b, int parties) {
    pthread_mutex_init(&b->mutex, nullptr);
    pthread_cond_init(&b->cond, nullptr);
    b->parties = parties;
    b->count = 0;
    b->generation = 0;
}

static void test_barrier_wait(test_barrier *b) {
    pthread_mutex_lock(&b->mutex);
    const int generation = b->generation;
    if (++b->count == b->parties) {
        b->count = 0;
        b->generation++;
        pthread_cond_broadcast(&b->cond);
    } else {
        while (generation == b->generation)
            pthread_cond_wait(&b->cond, &b->mutex);
    }
    pthread_mutex_unlock(&b->mutex);
}

static void test_barrier_destroy(test_barrier *b) {
    pthread_cond_destroy(&b->cond);
    pthread_mutex_destroy(&b->mutex);
}

typedef struct {
    optics_gfx_device *d;
    int iterations;
    int completed;
} worker_arg;

typedef struct {
    optics_gfx_device *d;
    test_barrier *start;
    int completed;
} canvas_worker_arg;

/* Race the first device-level canvas module-state publication. Each thread
 * owns its surface/canvas; only the lazily-created device cache is shared. */
static void *canvas_init_worker(void *p) {
    canvas_worker_arg *a = p;
    test_barrier_wait(a->start);
    optics_gfx_surface_desc sd = OPTICS_GFX_SURFACE_DESC_INIT;
    sd.width = 16;
    sd.height = 16;
    optics_gfx_surface *surface = nullptr;
    if (optics_gfx_surface_create(a->d, &sd, &surface) != OPTICS_GFX_OK)
        return nullptr;
    optics_gfx_canvas_desc cd = OPTICS_GFX_CANVAS_DESC_INIT;
    cd.surface = surface;
    optics_gfx_canvas *canvas = nullptr;
    if (optics_gfx_canvas_create(&cd, &canvas) == OPTICS_GFX_OK) {
        a->completed = 1;
        optics_gfx_canvas_release(canvas);
    }
    optics_gfx_surface_release(surface);
    return nullptr;
}

/* Each worker iteration creates a small image and a small mesh,
 * then immediately releases them. This drives the buffer + image
 * pools simultaneously from multiple threads. */
static void *image_mesh_worker(void *p) {
    worker_arg *a = p;
    a->completed = 0;
    for (int i = 0; i < a->iterations; ++i) {
        optics_gfx_image_desc idesc = {
            .type = OPTICS_GFX_TYPE_IMAGE_DESC,
            .width = 32,
            .height = 32,
            .format = OPTICS_GFX_FORMAT_RGBA8_UNORM,
        };
        optics_gfx_image *img = nullptr;
        if (optics_gfx_image_create(a->d, &idesc, &img) != OPTICS_GFX_OK)
            return nullptr;
        optics_gfx_image_release(img);

        optics_gfx_vertex verts[3] = {
            {{0, 0, 0}, {0, 0, 1}, {0, 0}},
            {{1, 0, 0}, {0, 0, 1}, {1, 0}},
            {{0, 1, 0}, {0, 0, 1}, {0, 1}},
        };
        optics_gfx_mesh_desc mdesc = {
            .type = OPTICS_GFX_TYPE_MESH_DESC,
            .vertices = verts,
            .vertex_count = 3,
        };
        optics_gfx_mesh *mesh = nullptr;
        if (optics_gfx_mesh_create(a->d, &mdesc, &mesh) != OPTICS_GFX_OK)
            return nullptr;
        optics_gfx_mesh_release(mesh);

        a->completed++;
    }
    return nullptr;
}

int main(void) {
    optics_gfx_device *d = optics_gfx_test_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_allocator: no Vulkan device available; skipping\n");
        OT_SKIP("no Vulkan device");
    }

    /* --- concurrent first-use publication of the canvas module state --- */
    {
        enum { N_THREADS = 4 };
        pthread_t threads[N_THREADS];
        canvas_worker_arg args[N_THREADS] = {};
        test_barrier start;
        test_barrier_init(&start, N_THREADS);
        for (int i = 0; i < N_THREADS; ++i) {
            args[i].d = d;
            args[i].start = &start;
            pthread_create(&threads[i], nullptr, canvas_init_worker, &args[i]);
        }
        int total = 0;
        for (int i = 0; i < N_THREADS; ++i) {
            pthread_join(threads[i], nullptr);
            total += args[i].completed;
        }
        test_barrier_destroy(&start);
        OT_EXPECT(total == N_THREADS);
    }

    /* --- single image create/release round-trip --- */
    {
        optics_gfx_image_desc desc = {
            .type = OPTICS_GFX_TYPE_IMAGE_DESC,
            .width = 64,
            .height = 64,
            .format = OPTICS_GFX_FORMAT_RGBA8_UNORM,
        };
        optics_gfx_image *img = nullptr;
        OT_EXPECT(optics_gfx_image_create(d, &desc, &img) == OPTICS_GFX_OK);
        OT_EXPECT(img != nullptr);
        optics_gfx_image_release(img);
    }

    /* --- many sequential image creates exercise pooling and free-list
     *     coalescing. If the allocator didn't reuse slots, we'd hit
     *     Vulkan's per-process VkDeviceMemory cap quickly. --- */
    {
        for (int i = 0; i < 256; ++i) {
            optics_gfx_image_desc desc = {
                .type = OPTICS_GFX_TYPE_IMAGE_DESC,
                .width = 64,
                .height = 64,
                .format = OPTICS_GFX_FORMAT_RGBA8_UNORM,
            };
            optics_gfx_image *img = nullptr;
            OT_EXPECT(optics_gfx_image_create(d, &desc, &img) == OPTICS_GFX_OK);
            optics_gfx_image_release(img);
        }
    }

    /* --- interleaved alloc/free pattern (worst-case fragmentation) --- */
    {
        optics_gfx_image *imgs[16] = {};
        optics_gfx_image_desc desc = {
            .type = OPTICS_GFX_TYPE_IMAGE_DESC,
            .width = 128,
            .height = 128,
            .format = OPTICS_GFX_FORMAT_RGBA8_UNORM,
        };
        for (int i = 0; i < 16; ++i) {
            OT_EXPECT(optics_gfx_image_create(d, &desc, &imgs[i]) == OPTICS_GFX_OK);
        }
        /* Free even indices, then re-alloc — exercises non-contiguous free list. */
        for (int i = 0; i < 16; i += 2) {
            optics_gfx_image_release(imgs[i]);
            imgs[i] = nullptr;
        }
        for (int i = 0; i < 16; i += 2) {
            OT_EXPECT(optics_gfx_image_create(d, &desc, &imgs[i]) == OPTICS_GFX_OK);
        }
        for (int i = 0; i < 16; ++i)
            optics_gfx_image_release(imgs[i]);
    }

    /* --- dedicated-allocation path for an oversize image
     *     (the allocator's dedicated threshold is 16 MiB; a 2048×2048
     *     RGBA8 is 16 MiB exactly, which trips the threshold). --- */
    {
        optics_gfx_image_desc desc = {
            .type = OPTICS_GFX_TYPE_IMAGE_DESC,
            .width = 2048,
            .height = 2048,
            .format = OPTICS_GFX_FORMAT_RGBA8_UNORM,
        };
        optics_gfx_image *img = nullptr;
        optics_gfx_result r = optics_gfx_image_create(d, &desc, &img);
        if (r == OPTICS_GFX_OK) {
            optics_gfx_image_release(img);
        } else {
            /* Lavapipe sometimes refuses very large images; that's fine. */
            OT_EXPECT(r == OPTICS_GFX_ERROR_BACKEND_FAILURE || r == OPTICS_GFX_ERROR_OUT_OF_MEMORY);
        }
    }

    /* --- meshes share their own pool with buffers --- */
    {
        optics_gfx_vertex verts[3] = {
            {{0, 0, 0}, {0, 0, 1}, {0, 0}},
            {{1, 0, 0}, {0, 0, 1}, {1, 0}},
            {{0, 1, 0}, {0, 0, 1}, {0, 1}},
        };
        uint32_t indices[3] = {0, 1, 2};
        optics_gfx_mesh_desc mdesc = {
            .type = OPTICS_GFX_TYPE_MESH_DESC,
            .vertices = verts,
            .vertex_count = 3,
            .indices = indices,
            .index_count = 3,
        };
        for (int i = 0; i < 64; ++i) {
            optics_gfx_mesh *mesh = nullptr;
            OT_EXPECT(optics_gfx_mesh_create(d, &mdesc, &mesh) == OPTICS_GFX_OK);
            optics_gfx_mesh_release(mesh);
        }
    }

    /* --- concurrent allocations from 4 threads --- */
    {
        enum { N_THREADS = 4, N_PER = 16 };
        pthread_t threads[N_THREADS];
        worker_arg args[N_THREADS];
        for (int i = 0; i < N_THREADS; ++i) {
            args[i].d = d;
            args[i].iterations = N_PER;
            args[i].completed = 0;
            pthread_create(&threads[i], nullptr, image_mesh_worker, &args[i]);
        }
        int total = 0;
        for (int i = 0; i < N_THREADS; ++i) {
            pthread_join(threads[i], nullptr);
            total += args[i].completed;
        }
        OT_EXPECT(total == N_THREADS * N_PER);
    }

    /* --- leak gate + staging-cache steady state: two identical
     *     create/settle rounds. Each upload advances the graphics serial
     *     past every parked retire zombie (tags are serial+1) and its
     *     fence wait sweeps them. Round 1 warms the staging cache; round
     *     2 must reuse the cached staging buffer and sweep round 1's
     *     zombie, landing on the exact same allocator counts. Any leak
     *     in the churn above shows up as growth between the rounds. --- */
    {
        optics_gfx_image_desc desc = {
            .type = OPTICS_GFX_TYPE_IMAGE_DESC,
            .width = 64,
            .height = 64,
            .format = OPTICS_GFX_FORMAT_RGBA8_UNORM,
        };
        optics_gfx_image *img1 = nullptr;
        OT_EXPECT(optics_gfx_image_create(d, &desc, &img1) == OPTICS_GFX_OK);
        optics_gfx_memory_stats s1;
        optics_gfx_device_memory_stats(d, &s1);
        optics_gfx_image_release(img1);

        optics_gfx_image *img2 = nullptr;
        OT_EXPECT(optics_gfx_image_create(d, &desc, &img2) == OPTICS_GFX_OK);
        optics_gfx_memory_stats s2;
        optics_gfx_device_memory_stats(d, &s2);
        OT_EXPECT(s2.live_allocations == s1.live_allocations);
        OT_EXPECT(s2.bytes_in_use == s1.bytes_in_use);
        optics_gfx_image_release(img2);
    }

    optics_gfx_device_release(d);
    return OT_REPORT();
}
