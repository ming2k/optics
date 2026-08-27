/*
 * flux_image_import_dmabuf: descriptor validation and extension-gating.
 *
 * This test does not require a real dma-buf producer. It verifies the
 * production-critical ownership rule on failures: flux must not close
 * caller-owned fds unless import succeeds.
 */
#include "test_helpers.h"
#include <flux/dmabuf.h>
#include <flux/flux.h>
#include <flux/vulkan.h>

#include <drm/drm_fourcc.h>
#include <fcntl.h>
#include <linux/dma-heap.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int try_dma_heap_alloc(size_t bytes) {
    int heap = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
    if (heap < 0)
        return -1;

    struct dma_heap_allocation_data data = {
        .len = bytes,
        .fd_flags = O_RDWR | O_CLOEXEC,
    };
    if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &data) != 0) {
        close(heap);
        return -1;
    }
    close(heap);
    return (int)data.fd;
}

static bool fd_is_open(int fd) {
    return fd >= 0 && fcntl(fd, F_GETFD) != -1;
}

int main(void) {
    EXPECT(flux_dmabuf_supported(nullptr) == false);
    EXPECT(flux_dmabuf_sync_supported(nullptr) == false);
    EXPECT(flux_image_import_dmabuf(nullptr, nullptr, nullptr) == FLUX_ERROR_INVALID_ARGUMENT);

    flux_device *d = test_helpers_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_dmabuf: no Vulkan device; skipping device checks\n");
        TEST_SUMMARY();
    }

    EXPECT(flux_dmabuf_supported(d) == false);
    EXPECT(flux_dmabuf_sync_supported(d) == false);

    flux_image *img = (flux_image *)0x1;
    flux_dmabuf_image_desc desc = FLUX_DMABUF_IMAGE_DESC_INIT;
    desc.width = 64;
    desc.height = 64;
    desc.format = FLUX_FORMAT_BGRA8_UNORM;
    desc.plane_count = 1;
    desc.planes[0].fd = -1;
    desc.planes[0].stride = 64 * 4;
    EXPECT(flux_image_import_dmabuf(d, &desc, &img) == FLUX_ERROR_INVALID_ARGUMENT);
    EXPECT(img == nullptr);

    desc.has_acquire_sync_fd = true;
    desc.acquire_sync_fd = -1;
    EXPECT(flux_image_import_dmabuf(d, &desc, &img) == FLUX_ERROR_INVALID_ARGUMENT);
    EXPECT(img == nullptr);
    desc.has_acquire_sync_fd = false;
    desc.acquire_sync_fd = 0;

    int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        desc.planes[0].fd = fd;
        EXPECT(flux_image_import_dmabuf(d, &desc, &img) == FLUX_ERROR_UNSUPPORTED);
        EXPECT(img == nullptr);
        EXPECT(fd_is_open(fd));
        close(fd);
    }

    fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        EXPECT(flux_canvas_wait_dmabuf_acquire(nullptr, nullptr, fd) ==
               FLUX_ERROR_INVALID_ARGUMENT);
        EXPECT(fd_is_open(fd));
        close(fd);
    }

    fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        desc.planes[0].fd = fd;
        desc.plane_count = 2;
        desc.planes[1].fd = fd;
        desc.planes[1].stride = 64 * 4;
        EXPECT(flux_image_import_dmabuf(d, &desc, &img) == FLUX_ERROR_UNSUPPORTED);
        EXPECT(img == nullptr);
        EXPECT(fd_is_open(fd));
        close(fd);
    }

    flux_device_release(d);

    flux_device *dd = test_helpers_make_dmabuf_device();
    if (dd) {
        EXPECT(flux_dmabuf_supported(dd) == true);

        flux_surface_desc sd = FLUX_SURFACE_DESC_INIT;
        sd.width = 16;
        sd.height = 16;
        flux_surface *surface = nullptr;

        flux_surface_dmabuf_desc invalid_export = FLUX_SURFACE_DMABUF_DESC_INIT;
        sd.next = &invalid_export;
        EXPECT(flux_surface_create(dd, &sd, &surface) == FLUX_ERROR_INVALID_ARGUMENT);
        EXPECT(surface == nullptr);

        uint64_t supported_modifiers[512];
        uint32_t supported_modifier_count = 512;
        EXPECT(flux_dmabuf_format_modifiers(dd, FLUX_FORMAT_BGRA8_UNORM, supported_modifiers,
                                            &supported_modifier_count) == FLUX_OK);
        flux_surface_dmabuf_desc exportable = FLUX_SURFACE_DMABUF_DESC_INIT;
        if (supported_modifier_count > 0) {
            exportable.modifiers = supported_modifiers;
            exportable.modifier_count = supported_modifier_count;
            sd.next = &exportable;
        } else {
            sd.next = nullptr;
        }

        flux_result sr = flux_surface_create(dd, &sd, &surface);
        if (sr == FLUX_OK) {
            if (flux_surface_exportable(surface)) {
                flux_frame *frame = nullptr;
                EXPECT(flux_surface_begin_frame(surface, nullptr, &frame) == FLUX_OK);
                EXPECT(flux_frame_submit(frame) == FLUX_OK);
                EXPECT(flux_frame_present(frame) == FLUX_OK);
                int exported = -1;
                int sync_fd = -1;
                if (flux_dmabuf_sync_supported(dd)) {
                    EXPECT(flux_surface_export_dmabuf_explicit(surface, &exported, &sync_fd) ==
                           FLUX_OK);
                    EXPECT(fd_is_open(sync_fd));
                } else {
                    EXPECT(flux_surface_export_dmabuf(surface, &exported) == FLUX_OK);
                }
                EXPECT(fd_is_open(exported));

                uint64_t produced_modifier = flux_surface_dmabuf_modifier(surface);
                if (exported >= 0) {
                    flux_dmabuf_image_desc imported_desc = FLUX_DMABUF_IMAGE_DESC_INIT;
                    imported_desc.width = 16;
                    imported_desc.height = 16;
                    imported_desc.format = FLUX_FORMAT_BGRA8_UNORM;
                    imported_desc.modifier = produced_modifier;
                    imported_desc.plane_count = 1;
                    imported_desc.planes[0].fd = exported;
                    imported_desc.planes[0].stride = flux_surface_dmabuf_stride(surface);
                    imported_desc.has_acquire_sync_fd = sync_fd >= 0;
                    imported_desc.acquire_sync_fd = sync_fd;

                    flux_image *sampled = nullptr;
                    flux_result imported_result =
                        flux_image_import_dmabuf(dd, &imported_desc, &sampled);
                    if (imported_result == FLUX_OK) {
                        EXPECT(!fd_is_open(exported));
                        if (sync_fd >= 0)
                            EXPECT(!fd_is_open(sync_fd));

                        flux_surface_desc consumer_desc = FLUX_SURFACE_DESC_INIT;
                        consumer_desc.width = 16;
                        consumer_desc.height = 16;
                        flux_surface *consumer = nullptr;
                        EXPECT(flux_surface_create(dd, &consumer_desc, &consumer) == FLUX_OK);
                        if (consumer) {
                            flux_canvas_desc canvas_desc = FLUX_CANVAS_DESC_INIT;
                            canvas_desc.surface = consumer;
                            flux_canvas *canvas = nullptr;
                            EXPECT(flux_canvas_create(&canvas_desc, &canvas) == FLUX_OK);
                            if (canvas) {
                                /* Repeated frames exercise release/reacquire
                                 * without rebuilding `sampled`. Eight frames
                                 * wrap the three frame slots twice, proving a
                                 * consumed temporary SYNC_FD payload can retire
                                 * behind the slot fence and its VkSemaphore can
                                 * be imported again from the pool. */
                                for (uint32_t frame_index = 0; frame_index < 8; ++frame_index) {
                                    int reuse_sync_fd = -1;
                                    if (frame_index > 0 && flux_dmabuf_sync_supported(dd)) {
                                        flux_frame *producer_frame = nullptr;
                                        EXPECT(flux_surface_begin_frame(
                                                   surface, nullptr, &producer_frame) == FLUX_OK);
                                        EXPECT(flux_frame_submit(producer_frame) == FLUX_OK);
                                        EXPECT(flux_frame_present(producer_frame) == FLUX_OK);
                                        int unused_dmabuf = -1;
                                        EXPECT(flux_surface_export_dmabuf_explicit(
                                                   surface, &unused_dmabuf, &reuse_sync_fd) ==
                                               FLUX_OK);
                                        EXPECT(fd_is_open(unused_dmabuf));
                                        EXPECT(fd_is_open(reuse_sync_fd));
                                        if (unused_dmabuf >= 0)
                                            close(unused_dmabuf);
                                    }

                                    flux_frame *consumer_frame = nullptr;
                                    EXPECT(flux_surface_begin_frame(consumer, nullptr,
                                                                    &consumer_frame) == FLUX_OK);
                                    flux_color clear = flux_color_rgba(0, 0, 0, 255);
                                    EXPECT(flux_canvas_begin_frame(canvas, consumer_frame, &clear) ==
                                           FLUX_OK);
                                    if (reuse_sync_fd >= 0) {
                                        EXPECT(flux_canvas_wait_dmabuf_acquire(
                                                   canvas, sampled, reuse_sync_fd) == FLUX_OK);
                                        EXPECT(!fd_is_open(reuse_sync_fd));
                                    }
                                    flux_canvas_draw_image(canvas, sampled,
                                                           (flux_rect){0, 0, 16, 16}, nullptr);
                                    flux_canvas_end_frame(canvas);
                                    EXPECT(flux_frame_submit(consumer_frame) == FLUX_OK);
                                    EXPECT(flux_frame_present(consumer_frame) == FLUX_OK);
                                }
                                flux_canvas_destroy(canvas);
                            }
                            flux_surface_release(consumer);
                        }
                        flux_image_release(sampled);
                    } else {
                        EXPECT(fd_is_open(exported));
                        close(exported);
                        if (sync_fd >= 0) {
                            EXPECT(fd_is_open(sync_fd));
                            close(sync_fd);
                        }
                    }
                }

                flux_surface_dmabuf_desc constrained = FLUX_SURFACE_DMABUF_DESC_INIT;
                constrained.modifiers = &produced_modifier;
                constrained.modifier_count = 1;
                sd.next = &constrained;
                flux_surface *shared = nullptr;
                EXPECT(flux_surface_create(dd, &sd, &shared) == FLUX_OK);
                EXPECT(shared != nullptr && flux_surface_exportable(shared));
                if (shared)
                    flux_surface_release(shared);
            }
            flux_surface_release(surface);
        }

        int dmabuf_fd = try_dma_heap_alloc(64u * 64u * 4u);
        if (dmabuf_fd >= 0) {
            /* Settle any zombies parked by the surface phase and take a
             * baseline: the probe's upload advances the graphics serial
             * past them, and keeping it alive pins exactly one known
             * allocation for the accounting assertions below. */
            flux_image_desc probe_desc = {
                .type = FLUX_TYPE_IMAGE_DESC,
                .width = 64,
                .height = 64,
                .format = FLUX_FORMAT_RGBA8_UNORM,
            };
            flux_image *probe = nullptr;
            EXPECT(flux_image_create(dd, &probe_desc, &probe) == FLUX_OK);
            flux_memory_stats before;
            flux_device_memory_stats(dd, &before);

            flux_dmabuf_image_desc real = FLUX_DMABUF_IMAGE_DESC_INIT;
            real.width = 64;
            real.height = 64;
            real.format = FLUX_FORMAT_BGRA8_UNORM;
            real.modifier = DRM_FORMAT_MOD_LINEAR;
            real.plane_count = 1;
            real.planes[0].fd = dmabuf_fd;
            real.planes[0].stride = 64 * 4;

            flux_image *imported = nullptr;
            flux_result rr = flux_image_import_dmabuf(dd, &real, &imported);
            if (rr == FLUX_OK) {
                EXPECT(imported != nullptr);
                EXPECT(!fd_is_open(dmabuf_fd));

                /* Imported memory bypasses the slab but must still be
                 * counted (ADR: stats and the teardown leak warning see
                 * external memory). */
                flux_memory_stats after_import;
                flux_device_memory_stats(dd, &after_import);
                EXPECT(after_import.live_allocations == before.live_allocations + 1);
                EXPECT(after_import.bytes_in_use > before.bytes_in_use);

                flux_image_release(imported);

                /* A fresh upload sweeps the import's retire zombie; the
                 * import's bytes must be uncounted again. */
                flux_image *tmp = nullptr;
                EXPECT(flux_image_create(dd, &probe_desc, &tmp) == FLUX_OK);
                flux_memory_stats settled;
                flux_device_memory_stats(dd, &settled);
                EXPECT(settled.live_allocations == before.live_allocations + 1);
                EXPECT(settled.bytes_in_use < after_import.bytes_in_use);
                flux_image_release(tmp);
            } else {
                EXPECT(imported == nullptr);
                EXPECT(fd_is_open(dmabuf_fd));
                close(dmabuf_fd);
            }
            flux_image_release(probe);
        } else {
            fprintf(stderr,
                    "test_dmabuf: /dev/dma_heap/system unavailable; skipping real import\n");
        }
        flux_device_release(dd);
    } else {
        fprintf(stderr, "test_dmabuf: dmabuf extensions unavailable; skipping real import\n");
    }

    TEST_SUMMARY();
}
