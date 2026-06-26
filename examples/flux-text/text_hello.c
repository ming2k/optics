/*
 * text_hello — shape and draw a UTF-8 run through flux-text.
 *
 * Demonstrates the ADR-0016 boundary: the app owns the window, device,
 * surface, canvas, and arena; flux-text owns shaping (HarfBuzz) and feeds
 * the flux_canvas_draw_glyph_run primitive. BiDi and CJK work because
 * FriBidi + HarfBuzz live in the sibling, not in libflux.
 */
#include <flux-text/text.h>
#include <flux/canvas.h>
#include <flux/flux.h>
#include <flux/vulkan.h>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <stdio.h>
#include <string.h>

static void on_resize(GLFWwindow *win, int w, int h) {
    flux_surface *surface = glfwGetWindowUserPointer(win);
    if (surface && w > 0 && h > 0)
        (void)flux_surface_resize(surface, (uint32_t)w, (uint32_t)h);
}

int main(void) {
    if (!glfwInit() || !glfwVulkanSupported())
        return 1;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    GLFWwindow *win = glfwCreateWindow(800, 480, "flux text", nullptr, nullptr);
    if (!win) {
        glfwTerminate();
        return 1;
    }

    uint32_t ext_count = 0;
    const char **req_exts = glfwGetRequiredInstanceExtensions(&ext_count);
    const char *device_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    flux_device_desc ddesc = {
        .type = FLUX_TYPE_DEVICE_DESC,
        .log = flux_console_logger,
        .validation = FLUX_VALIDATION_AUTO,
        .required_instance_extensions = req_exts,
        .required_instance_extension_count = ext_count,
        .required_device_extensions = device_exts,
        .required_device_extension_count = sizeof(device_exts) / sizeof(*device_exts),
        .frames_in_flight = 2,
    };
    flux_device *device = nullptr;
    if (flux_device_create(&ddesc, &device) != FLUX_OK) {
        glfwDestroyWindow(win);
        glfwTerminate();
        return 1;
    }

    VkSurfaceKHR vk_surface = VK_NULL_HANDLE;
    if (glfwCreateWindowSurface(flux_device_vk_instance(device), win, nullptr, &vk_surface) !=
        VK_SUCCESS) {
        flux_device_release(device);
        glfwDestroyWindow(win);
        glfwTerminate();
        return 1;
    }
    int fbw = 0, fbh = 0;
    glfwGetFramebufferSize(win, &fbw, &fbh);
    flux_surface_desc sdesc = {
        .type = FLUX_TYPE_SURFACE_DESC,
        .vk_surface_khr = vk_surface,
        .width = (uint32_t)fbw,
        .height = (uint32_t)fbh,
        .vsync = true,
    };
    flux_surface *surface = nullptr;
    if (flux_surface_create(device, &sdesc, &surface) != FLUX_OK) {
        vkDestroySurfaceKHR(flux_device_vk_instance(device), vk_surface, nullptr);
        flux_device_release(device);
        glfwDestroyWindow(win);
        glfwTerminate();
        return 1;
    }
    glfwSetWindowUserPointer(win, surface);
    glfwSetFramebufferSizeCallback(win, on_resize);

    flux_canvas_desc cdesc = {
        .type = FLUX_TYPE_CANVAS_DESC,
        .surface = surface,
        .scale = 1.0f,
    };
    flux_canvas *canvas = nullptr;
    if (flux_canvas_create(&cdesc, &canvas) != FLUX_OK)
        goto teardown;

    flux_text_desc tdesc = {.device = device, .scale = 1.0f};
    flux_text *text = nullptr;
    if (flux_text_create(&tdesc, &text) != FLUX_OK) {
        fprintf(stderr, "flux_text_create failed (is fontconfig available?)\n");
        goto teardown;
    }

    flux_arena arena_store;
    if (flux_arena_init(&arena_store, 1u << 20, nullptr) != FLUX_OK) {
        flux_text_destroy(text);
        goto teardown;
    }

    printf("text_hello ready\n");
    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();
        flux_frame *frame = nullptr;
        flux_result r = flux_surface_begin_frame(surface, nullptr, &frame);
        if (r == FLUX_ERROR_SURFACE_LOST) {
            int w, h;
            glfwGetFramebufferSize(win, &w, &h);
            if (w > 0 && h > 0)
                (void)flux_surface_resize(surface, (uint32_t)w, (uint32_t)h);
            continue;
        }
        if (r == FLUX_ERROR_INVALID_STATE)
            continue;
        if (r != FLUX_OK)
            break;

        flux_color clear = flux_color_rgba(0x12, 0x14, 0x1a, 0xff);
        if (flux_canvas_begin(canvas, frame, &clear) == FLUX_OK) {
            const char *s = "hello, flux-text \xe4\xbd\xa0\xe5\xa5\xbd"; /* + "你好" */
            flux_text_style style = {
                .size_px = 40.0f,
                .weight = 0.0f,
                .color = flux_color_rgba(0xe8, 0xe8, 0xe8, 0xff),
                .family = FLUX_TEXT_FAMILY_SANS,
            };
            flux_text_draw(text, canvas, &arena_store, 40.0f, 80.0f, s, strlen(s), &style);
            flux_canvas_end(canvas);
        }

        r = flux_frame_submit(frame);
        if (r != FLUX_OK)
            break;
        r = flux_frame_present(frame);
        if (r == FLUX_ERROR_SURFACE_LOST) {
            int w, h;
            glfwGetFramebufferSize(win, &w, &h);
            if (w > 0 && h > 0)
                (void)flux_surface_resize(surface, (uint32_t)w, (uint32_t)h);
        } else if (r != FLUX_OK)
            break;
        flux_arena_reset(&arena_store);
    }

    flux_device_wait_idle(device);
    flux_arena_destroy(&arena_store);
    flux_text_destroy(text);
teardown:
    if (canvas)
        flux_canvas_destroy(canvas);
    flux_surface_release(surface);
    vkDestroySurfaceKHR(flux_device_vk_instance(device), vk_surface, nullptr);
    flux_device_release(device);
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
