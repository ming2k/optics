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
#include "test_helpers.h"
#include <flux/flux.h>
#include <flux/scene.h>
#include <flux/vulkan.h>

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    flux_device *d;
    int iterations;
    int completed;
} worker_arg;

typedef struct {
    flux_device *d;
    pthread_barrier_t *start;
    int completed;
} canvas_worker_arg;

/* Race the first device-level canvas module-state publication. Each thread
 * owns its surface/canvas; only the lazily-created device cache is shared. */
static void *canvas_init_worker(void *p) {
    canvas_worker_arg *a = p;
    pthread_barrier_wait(a->start);
    flux_surface_desc sd = FLUX_SURFACE_DESC_INIT;
    sd.width = 16;
    sd.height = 16;
    flux_surface *surface = NULL;
    if (flux_surface_create(a->d, &sd, &surface) != FLUX_OK)
        return NULL;
    flux_canvas_desc cd = FLUX_CANVAS_DESC_INIT;
    cd.surface = surface;
    flux_canvas *canvas = NULL;
    if (flux_canvas_create(&cd, &canvas) == FLUX_OK) {
        a->completed = 1;
        flux_canvas_destroy(canvas);
    }
    flux_surface_release(surface);
    return NULL;
}

/* Each worker iteration creates a small image and a small mesh,
 * then immediately releases them. This drives the buffer + image
 * pools simultaneously from multiple threads. */
static void *image_mesh_worker(void *p) {
    worker_arg *a = p;
    a->completed = 0;
    for (int i = 0; i < a->iterations; ++i) {
        flux_image_desc idesc = {
            .type = FLUX_TYPE_IMAGE_DESC,
            .width = 32,
            .height = 32,
            .format = FLUX_FORMAT_RGBA8_UNORM,
        };
        flux_image *img = NULL;
        if (flux_image_create(a->d, &idesc, &img) != FLUX_OK)
            return NULL;
        flux_image_release(img);

        flux_vertex verts[3] = {
            {{0, 0, 0}, {0, 0, 1}, {0, 0}},
            {{1, 0, 0}, {0, 0, 1}, {1, 0}},
            {{0, 1, 0}, {0, 0, 1}, {0, 1}},
        };
        flux_mesh_desc mdesc = {
            .type = FLUX_TYPE_MESH_DESC,
            .vertices = verts,
            .vertex_count = 3,
        };
        flux_mesh *mesh = NULL;
        if (flux_mesh_create(a->d, &mdesc, &mesh) != FLUX_OK)
            return NULL;
        flux_mesh_release(mesh);

        a->completed++;
    }
    return NULL;
}

int main(void) {
    flux_device *d = test_helpers_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_allocator: no Vulkan device available; skipping\n");
        return 0;
    }

    /* --- concurrent first-use publication of the canvas module state --- */
    {
        enum { N_THREADS = 4 };
        pthread_t threads[N_THREADS];
        canvas_worker_arg args[N_THREADS] = {0};
        pthread_barrier_t start;
        pthread_barrier_init(&start, NULL, N_THREADS);
        for (int i = 0; i < N_THREADS; ++i) {
            args[i].d = d;
            args[i].start = &start;
            pthread_create(&threads[i], NULL, canvas_init_worker, &args[i]);
        }
        int total = 0;
        for (int i = 0; i < N_THREADS; ++i) {
            pthread_join(threads[i], NULL);
            total += args[i].completed;
        }
        pthread_barrier_destroy(&start);
        EXPECT(total == N_THREADS);
    }

    /* --- single image create/release round-trip --- */
    {
        flux_image_desc desc = {
            .type = FLUX_TYPE_IMAGE_DESC,
            .width = 64,
            .height = 64,
            .format = FLUX_FORMAT_RGBA8_UNORM,
        };
        flux_image *img = NULL;
        EXPECT(flux_image_create(d, &desc, &img) == FLUX_OK);
        EXPECT(img != NULL);
        flux_image_release(img);
    }

    /* --- many sequential image creates exercise pooling and free-list
     *     coalescing. If the allocator didn't reuse slots, we'd hit
     *     Vulkan's per-process VkDeviceMemory cap quickly. --- */
    {
        for (int i = 0; i < 256; ++i) {
            flux_image_desc desc = {
                .type = FLUX_TYPE_IMAGE_DESC,
                .width = 64,
                .height = 64,
                .format = FLUX_FORMAT_RGBA8_UNORM,
            };
            flux_image *img = NULL;
            EXPECT(flux_image_create(d, &desc, &img) == FLUX_OK);
            flux_image_release(img);
        }
    }

    /* --- interleaved alloc/free pattern (worst-case fragmentation) --- */
    {
        flux_image *imgs[16] = {0};
        flux_image_desc desc = {
            .type = FLUX_TYPE_IMAGE_DESC,
            .width = 128,
            .height = 128,
            .format = FLUX_FORMAT_RGBA8_UNORM,
        };
        for (int i = 0; i < 16; ++i) {
            EXPECT(flux_image_create(d, &desc, &imgs[i]) == FLUX_OK);
        }
        /* Free even indices, then re-alloc — exercises non-contiguous free list. */
        for (int i = 0; i < 16; i += 2) {
            flux_image_release(imgs[i]);
            imgs[i] = NULL;
        }
        for (int i = 0; i < 16; i += 2) {
            EXPECT(flux_image_create(d, &desc, &imgs[i]) == FLUX_OK);
        }
        for (int i = 0; i < 16; ++i)
            flux_image_release(imgs[i]);
    }

    /* --- dedicated-allocation path for an oversize image
     *     (the allocator's dedicated threshold is 16 MiB; a 2048×2048
     *     RGBA8 is 16 MiB exactly, which trips the threshold). --- */
    {
        flux_image_desc desc = {
            .type = FLUX_TYPE_IMAGE_DESC,
            .width = 2048,
            .height = 2048,
            .format = FLUX_FORMAT_RGBA8_UNORM,
        };
        flux_image *img = NULL;
        flux_result r = flux_image_create(d, &desc, &img);
        if (r == FLUX_OK) {
            flux_image_release(img);
        } else {
            /* Lavapipe sometimes refuses very large images; that's fine. */
            EXPECT(r == FLUX_ERROR_BACKEND_FAILURE || r == FLUX_ERROR_OUT_OF_MEMORY);
        }
    }

    /* --- meshes share their own pool with buffers --- */
    {
        flux_vertex verts[3] = {
            {{0, 0, 0}, {0, 0, 1}, {0, 0}},
            {{1, 0, 0}, {0, 0, 1}, {1, 0}},
            {{0, 1, 0}, {0, 0, 1}, {0, 1}},
        };
        uint32_t indices[3] = {0, 1, 2};
        flux_mesh_desc mdesc = {
            .type = FLUX_TYPE_MESH_DESC,
            .vertices = verts,
            .vertex_count = 3,
            .indices = indices,
            .index_count = 3,
        };
        for (int i = 0; i < 64; ++i) {
            flux_mesh *mesh = NULL;
            EXPECT(flux_mesh_create(d, &mdesc, &mesh) == FLUX_OK);
            flux_mesh_release(mesh);
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
            pthread_create(&threads[i], NULL, image_mesh_worker, &args[i]);
        }
        int total = 0;
        for (int i = 0; i < N_THREADS; ++i) {
            pthread_join(threads[i], NULL);
            total += args[i].completed;
        }
        EXPECT(total == N_THREADS * N_PER);
    }

    flux_device_release(d);
    TEST_SUMMARY();
}
