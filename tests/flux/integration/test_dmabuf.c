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
        int dmabuf_fd = try_dma_heap_alloc(64u * 64u * 4u);
        if (dmabuf_fd >= 0) {
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
                flux_image_release(imported);
            } else {
                EXPECT(imported == nullptr);
                EXPECT(fd_is_open(dmabuf_fd));
                close(dmabuf_fd);
            }
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
