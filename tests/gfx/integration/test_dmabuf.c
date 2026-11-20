/*
 * flux_image_import_dmabuf: descriptor validation and extension-gating.
 *
 * This test does not require a real dma-buf producer. It verifies the
 * production-critical ownership rule on failures: flux must not close
 * caller-owned fds unless import succeeds.
 */
#include "gfx_test.h"
#include "optics_test.h"
#include <optics/gfx.h>
#include <optics/gfx/dmabuf.h>
#include <optics/gfx/vulkan.h>

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
    bool skipped_real_import = false;
    OT_EXPECT(optics_gfx_dmabuf_supported(nullptr) == false);
    OT_EXPECT(optics_gfx_dmabuf_sync_supported(nullptr) == false);
    OT_EXPECT(optics_gfx_image_import_dmabuf(nullptr, nullptr, nullptr) ==
              OPTICS_GFX_ERROR_INVALID_ARGUMENT);

    optics_gfx_device *d = optics_gfx_test_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_dmabuf: no Vulkan device; skipping device checks\n");
        OT_SKIP("no Vulkan device");
    }

    OT_EXPECT(optics_gfx_dmabuf_supported(d) == false);
    OT_EXPECT(optics_gfx_dmabuf_sync_supported(d) == false);

    optics_gfx_image *img = (optics_gfx_image *)0x1;
    optics_gfx_dmabuf_image_desc desc = OPTICS_GFX_DMABUF_IMAGE_DESC_INIT;
    desc.width = 64;
    desc.height = 64;
    desc.format = OPTICS_GFX_FORMAT_BGRA8_UNORM;
    desc.plane_count = 1;
    desc.planes[0].fd = -1;
    desc.planes[0].stride = 64 * 4;
    OT_EXPECT(optics_gfx_image_import_dmabuf(d, &desc, &img) == OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(img == nullptr);

    desc.has_acquire_sync_fd = true;
    desc.acquire_sync_fd = -1;
    OT_EXPECT(optics_gfx_image_import_dmabuf(d, &desc, &img) == OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(img == nullptr);
    desc.has_acquire_sync_fd = false;
    desc.acquire_sync_fd = 0;

    int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        desc.planes[0].fd = fd;
        OT_EXPECT(optics_gfx_image_import_dmabuf(d, &desc, &img) == OPTICS_GFX_ERROR_UNSUPPORTED);
        OT_EXPECT(img == nullptr);
        OT_EXPECT(fd_is_open(fd));
        close(fd);
    }

    fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        OT_EXPECT(optics_gfx_canvas_wait_dmabuf_acquire(
                      nullptr, &(optics_gfx_canvas_wait_dmabuf_acquire_opts){
                                   .image = nullptr,
                                   .acquire_sync_fd = fd}) == OPTICS_GFX_ERROR_INVALID_ARGUMENT);
        OT_EXPECT(fd_is_open(fd));
        close(fd);
    }

    fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        desc.planes[0].fd = fd;
        desc.plane_count = 2;
        desc.planes[1].fd = fd;
        desc.planes[1].stride = 64 * 4;
        OT_EXPECT(optics_gfx_image_import_dmabuf(d, &desc, &img) == OPTICS_GFX_ERROR_UNSUPPORTED);
        OT_EXPECT(img == nullptr);
        OT_EXPECT(fd_is_open(fd));
        close(fd);
    }

    optics_gfx_device_release(d);

    optics_gfx_device *dd = optics_gfx_test_make_dmabuf_device();
    if (dd) {
        OT_EXPECT(optics_gfx_dmabuf_supported(dd) == true);

        optics_gfx_surface_desc sd = OPTICS_GFX_SURFACE_DESC_INIT;
        sd.width = 16;
        sd.height = 16;
        optics_gfx_surface *surface = nullptr;

        optics_gfx_surface_dmabuf_desc invalid_export = OPTICS_GFX_SURFACE_DMABUF_DESC_INIT;
        sd.next = &invalid_export;
        OT_EXPECT(optics_gfx_surface_create(dd, &sd, &surface) ==
                  OPTICS_GFX_ERROR_INVALID_ARGUMENT);
        OT_EXPECT(surface == nullptr);

        uint64_t supported_modifiers[512];
        uint32_t supported_modifier_count = 512;
        OT_EXPECT(optics_gfx_dmabuf_format_modifiers(
                      dd, &(optics_gfx_dmabuf_format_modifiers_opts){
                              .format = OPTICS_GFX_FORMAT_BGRA8_UNORM,
                              .out_modifiers = supported_modifiers,
                              .inout_count = &supported_modifier_count}) == OPTICS_GFX_OK);
        optics_gfx_surface_dmabuf_desc exportable = OPTICS_GFX_SURFACE_DMABUF_DESC_INIT;
        if (supported_modifier_count > 0) {
            exportable.modifiers = supported_modifiers;
            exportable.modifier_count = supported_modifier_count;
            sd.next = &exportable;
        } else {
            sd.next = nullptr;
        }

        optics_gfx_result sr = optics_gfx_surface_create(dd, &sd, &surface);
        if (sr == OPTICS_GFX_OK) {
            if (optics_gfx_surface_exportable(surface)) {
                /* An abandoned submission (ADR-0110) releases the frame-slot
                 * borrow without advancing the offscreen ring, so the very next
                 * begin_frame reuses the *same* slot. On an exportable surface
                 * that slot's SYNC_FD export semaphore was signalled by the
                 * abandoned submit and never had its payload exported: the
                 * begin must reset it, or the retry's vkQueueSubmit2 would start
                 * from an already-signalled binary semaphore. */
                optics_gfx_frame *abandoned = nullptr;
                OT_EXPECT(optics_gfx_surface_begin_frame(surface, nullptr, &abandoned) ==
                          OPTICS_GFX_OK);
                OT_EXPECT(optics_gfx_frame_submit(abandoned) == OPTICS_GFX_OK);
                optics_gfx_frame_abandon(abandoned);
                /* Abandon consumed the frame: later transitions reject. */
                OT_EXPECT(optics_gfx_frame_present(abandoned) == OPTICS_GFX_ERROR_INVALID_STATE);

                optics_gfx_frame *retry = nullptr;
                OT_EXPECT(optics_gfx_surface_begin_frame(surface, nullptr, &retry) ==
                          OPTICS_GFX_OK);
                OT_EXPECT(optics_gfx_frame_submit(retry) == OPTICS_GFX_OK);
                OT_EXPECT(optics_gfx_frame_present(retry) == OPTICS_GFX_OK);

                optics_gfx_frame *frame = nullptr;
                OT_EXPECT(optics_gfx_surface_begin_frame(surface, nullptr, &frame) ==
                          OPTICS_GFX_OK);
                OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
                OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
                int exported = -1;
                int sync_fd = -1;
                optics_gfx_surface_dmabuf_lease *lease = nullptr;
                if (optics_gfx_dmabuf_sync_supported(dd)) {
                    OT_EXPECT(optics_gfx_surface_export_dmabuf_explicit(
                                  surface, &exported, &sync_fd, &lease) == OPTICS_GFX_OK);
                    OT_EXPECT(fd_is_open(sync_fd));
                } else {
                    OT_EXPECT(optics_gfx_surface_export_dmabuf(surface, &exported, &lease) ==
                              OPTICS_GFX_OK);
                }
                OT_EXPECT(lease != nullptr);
                OT_EXPECT(fd_is_open(exported));

                uint64_t produced_modifier = optics_gfx_surface_dmabuf_modifier(surface);
                if (exported >= 0) {
                    optics_gfx_dmabuf_image_desc imported_desc = OPTICS_GFX_DMABUF_IMAGE_DESC_INIT;
                    imported_desc.width = 16;
                    imported_desc.height = 16;
                    imported_desc.format = OPTICS_GFX_FORMAT_BGRA8_UNORM;
                    imported_desc.modifier = produced_modifier;
                    imported_desc.plane_count = 1;
                    imported_desc.planes[0].fd = exported;
                    imported_desc.planes[0].stride = optics_gfx_surface_dmabuf_stride(surface);
                    imported_desc.has_acquire_sync_fd = sync_fd >= 0;
                    imported_desc.acquire_sync_fd = sync_fd;

                    optics_gfx_image *sampled = nullptr;
                    optics_gfx_result imported_result =
                        optics_gfx_image_import_dmabuf(dd, &imported_desc, &sampled);
                    if (imported_result == OPTICS_GFX_OK) {
                        OT_EXPECT(!fd_is_open(exported));
                        if (sync_fd >= 0)
                            OT_EXPECT(!fd_is_open(sync_fd));

                        /* A borrowed import may be sampled now but must not be
                         * frozen into a retained DisplayList; dynamic input
                         * slots are the supported path. */
                        optics_gfx_image *borrowed_snapshot = nullptr;
                        OT_EXPECT(optics_gfx_image_snapshot(sampled, &borrowed_snapshot) ==
                                  OPTICS_GFX_ERROR_INVALID_STATE);
                        OT_EXPECT(borrowed_snapshot == nullptr);

                        optics_gfx_surface_desc consumer_desc = OPTICS_GFX_SURFACE_DESC_INIT;
                        consumer_desc.width = 16;
                        consumer_desc.height = 16;
                        optics_gfx_surface *consumer = nullptr;
                        OT_EXPECT(optics_gfx_surface_create(dd, &consumer_desc, &consumer) ==
                                  OPTICS_GFX_OK);
                        if (consumer) {
                            optics_gfx_canvas_desc canvas_desc = OPTICS_GFX_CANVAS_DESC_INIT;
                            canvas_desc.surface = consumer;
                            optics_gfx_canvas *canvas = nullptr;
                            OT_EXPECT(optics_gfx_canvas_create(&canvas_desc, &canvas) ==
                                      OPTICS_GFX_OK);
                            if (canvas) {
                                /* Repeated frames exercise release/reacquire
                                 * without rebuilding `sampled`. Eight frames
                                 * wrap the three frame slots twice, proving a
                                 * consumed temporary SYNC_FD payload can retire
                                 * behind the slot fence and its VkSemaphore can
                                 * be imported again from the pool. */
                                optics_gfx_surface_dmabuf_lease_release(lease);
                                lease = nullptr;
                                for (uint32_t frame_index = 0; frame_index < 8; ++frame_index) {
                                    int reuse_sync_fd = -1;
                                    if (frame_index > 0 && optics_gfx_dmabuf_sync_supported(dd)) {
                                        optics_gfx_frame *producer_frame = nullptr;
                                        OT_EXPECT(optics_gfx_surface_begin_frame(surface, nullptr,
                                                                                 &producer_frame) ==
                                                  OPTICS_GFX_OK);
                                        OT_EXPECT(optics_gfx_frame_submit(producer_frame) ==
                                                  OPTICS_GFX_OK);
                                        OT_EXPECT(optics_gfx_frame_present(producer_frame) ==
                                                  OPTICS_GFX_OK);
                                        int unused_dmabuf = -1;
                                        optics_gfx_surface_dmabuf_lease *unused_lease = nullptr;
                                        OT_EXPECT(optics_gfx_surface_export_dmabuf_explicit(
                                                      surface, &unused_dmabuf, &reuse_sync_fd,
                                                      &unused_lease) == OPTICS_GFX_OK);
                                        OT_EXPECT(fd_is_open(unused_dmabuf));
                                        OT_EXPECT(fd_is_open(reuse_sync_fd));
                                        if (unused_dmabuf >= 0)
                                            close(unused_dmabuf);
                                        optics_gfx_surface_dmabuf_lease_release(unused_lease);
                                    }

                                    optics_gfx_frame *consumer_frame = nullptr;
                                    OT_EXPECT(optics_gfx_surface_begin_frame(consumer, nullptr,
                                                                             &consumer_frame) ==
                                              OPTICS_GFX_OK);
                                    optics_gfx_color clear = optics_gfx_color_rgba(0, 0, 0, 255);
                                    OT_EXPECT(
                                        optics_gfx_canvas_begin(
                                            canvas, &(optics_gfx_canvas_pass_desc){
                                                        .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                        .frame = consumer_frame,
                                                        .clear_color = &clear}) == OPTICS_GFX_OK);
                                    if (reuse_sync_fd >= 0) {
                                        OT_EXPECT(optics_gfx_canvas_wait_dmabuf_acquire(
                                                      canvas,
                                                      &(optics_gfx_canvas_wait_dmabuf_acquire_opts){
                                                          .image = sampled,
                                                          .acquire_sync_fd = reuse_sync_fd}) ==
                                                  OPTICS_GFX_OK);
                                        OT_EXPECT(!fd_is_open(reuse_sync_fd));
                                    }
                                    optics_gfx_canvas_draw_image(
                                        canvas, sampled, (optics_gfx_rect){0, 0, 16, 16}, nullptr);
                                    OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
                                    OT_EXPECT(optics_gfx_frame_submit(consumer_frame) ==
                                              OPTICS_GFX_OK);
                                    OT_EXPECT(optics_gfx_frame_present(consumer_frame) ==
                                              OPTICS_GFX_OK);
                                }
                                optics_gfx_canvas_release(canvas);
                            }
                            optics_gfx_surface_release(consumer);
                        }
                        optics_gfx_image_release(sampled);
                    } else {
                        OT_EXPECT(fd_is_open(exported));
                        close(exported);
                        if (sync_fd >= 0) {
                            OT_EXPECT(fd_is_open(sync_fd));
                            close(sync_fd);
                        }
                    }
                }

                optics_gfx_surface_dmabuf_desc constrained = OPTICS_GFX_SURFACE_DMABUF_DESC_INIT;
                constrained.modifiers = &produced_modifier;
                constrained.modifier_count = 1;
                sd.next = &constrained;
                optics_gfx_surface *shared = nullptr;
                OT_EXPECT(optics_gfx_surface_create(dd, &sd, &shared) == OPTICS_GFX_OK);
                OT_EXPECT(shared != nullptr && optics_gfx_surface_exportable(shared));
                if (shared)
                    optics_gfx_surface_release(shared);
            }
            optics_gfx_surface_release(surface);
        }

        int dmabuf_fd = try_dma_heap_alloc(64u * 64u * 4u);
        if (dmabuf_fd >= 0) {
            /* Settle any zombies parked by the surface phase and take a
             * baseline: the probe's upload advances the graphics serial
             * past them, and keeping it alive pins exactly one known
             * allocation for the accounting assertions below. */
            optics_gfx_image_desc probe_desc = {
                .type = OPTICS_GFX_TYPE_IMAGE_DESC,
                .width = 64,
                .height = 64,
                .format = OPTICS_GFX_FORMAT_RGBA8_UNORM,
            };
            optics_gfx_image *probe = nullptr;
            OT_EXPECT(optics_gfx_image_create(dd, &probe_desc, &probe) == OPTICS_GFX_OK);
            optics_gfx_memory_stats before;
            optics_gfx_device_memory_stats(dd, &before);

            optics_gfx_dmabuf_image_desc real = OPTICS_GFX_DMABUF_IMAGE_DESC_INIT;
            real.width = 64;
            real.height = 64;
            real.format = OPTICS_GFX_FORMAT_BGRA8_UNORM;
            real.modifier = DRM_FORMAT_MOD_LINEAR;
            real.plane_count = 1;
            real.planes[0].fd = dmabuf_fd;
            real.planes[0].stride = 64 * 4;

            optics_gfx_image *imported = nullptr;
            optics_gfx_result rr = optics_gfx_image_import_dmabuf(dd, &real, &imported);
            if (rr == OPTICS_GFX_OK) {
                OT_EXPECT(imported != nullptr);
                OT_EXPECT(!fd_is_open(dmabuf_fd));

                /* Imported memory bypasses the slab but must still be
                 * counted (ADR: stats and the teardown leak warning see
                 * external memory). */
                optics_gfx_memory_stats after_import;
                optics_gfx_device_memory_stats(dd, &after_import);
                OT_EXPECT(after_import.live_allocations == before.live_allocations + 1);
                OT_EXPECT(after_import.bytes_in_use > before.bytes_in_use);

                optics_gfx_image_release(imported);

                /* A fresh upload sweeps the import's retire zombie; the
                 * import's bytes must be uncounted again. */
                optics_gfx_image *tmp = nullptr;
                OT_EXPECT(optics_gfx_image_create(dd, &probe_desc, &tmp) == OPTICS_GFX_OK);
                optics_gfx_memory_stats settled;
                optics_gfx_device_memory_stats(dd, &settled);
                OT_EXPECT(settled.live_allocations == before.live_allocations + 1);
                OT_EXPECT(settled.bytes_in_use < after_import.bytes_in_use);
                optics_gfx_image_release(tmp);
            } else {
                OT_EXPECT(imported == nullptr);
                OT_EXPECT(fd_is_open(dmabuf_fd));
                close(dmabuf_fd);
            }
            optics_gfx_image_release(probe);
        } else {
            skipped_real_import = true;
            fprintf(stderr,
                    "test_dmabuf: /dev/dma_heap/system unavailable; skipping real import\n");
        }
        optics_gfx_device_release(dd);
    } else {
        skipped_real_import = true;
        fprintf(stderr, "test_dmabuf: dmabuf extensions unavailable; skipping real import\n");
    }

    if (skipped_real_import)
        OT_SKIP("dmabuf real import unavailable");
    return OT_REPORT();
}
