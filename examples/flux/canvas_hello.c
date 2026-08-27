/*
 * canvas_hello — the 2D canvas, end to end.
 *
 * A single-file tour of the immediate-mode canvas. Every draw call below
 * runs once per frame between flux_canvas_begin_frame and flux_canvas_end_frame;
 * paths are allocated from a per-frame arena that is reset each frame.
 *
 * Teaches:
 *   - solid fills with premultiplied SRC_OVER blending
 *   - linear + radial gradients (flux_paint_*_gradient)
 *   - filled paths: rounded rects, circles, a concave star (tessellator)
 *   - stroked paths with caps, joins, and a miter limit
 *   - image draws from a flux_image (procedural checker texture)
 *   - the state stack (save / translate / rotate / restore)
 * Key flux APIs:  flux_canvas_create/_begin/_end, flux_canvas_fill_rect[_color],
 *                 flux_canvas_fill_path, flux_canvas_stroke_path,
 *                 flux_canvas_draw_image, flux_path_*, flux_paint_*
 * Plumbing (raw Vulkan, not flux): GLFW window + VkSurfaceKHR creation.
 */
#include <flux/flux.h>
#include <flux/vulkan.h>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "pipeline_cache.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void on_resize(GLFWwindow *win, int w, int h) {
    flux_surface *surface = glfwGetWindowUserPointer(win);
    if (surface && w > 0 && h > 0)
        (void)flux_surface_resize(surface, (uint32_t)w, (uint32_t)h);
}

int main(void) {
    if (!glfwInit() || !glfwVulkanSupported()) {
        fprintf(stderr, "glfw vk init failed\n");
        return 1;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    GLFWwindow *win = glfwCreateWindow(960, 540, "flux canvas", nullptr, nullptr);
    if (!win) {
        glfwTerminate();
        return 1;
    }

    uint32_t ext_count = 0;
    const char **req_exts = glfwGetRequiredInstanceExtensions(&ext_count);
    const char *device_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    flux_pipeline_cache_file cache = FLUX_PIPELINE_CACHE_FILE_INIT;
    flux_pipeline_cache_file_set_default_path(&cache, "canvas_hello.bin");

    flux_device_desc ddesc = {
        .type = FLUX_TYPE_DEVICE_DESC,
        .log = flux_console_logger,
        .validation = FLUX_VALIDATION_AUTO,
        .required_instance_extensions = req_exts,
        .required_instance_extension_count = ext_count,
        .required_device_extensions = device_exts,
        .required_device_extension_count = sizeof(device_exts) / sizeof(*device_exts),
        .frames_in_flight = 2,
        .pipeline_cache_load = flux_pipeline_cache_file_load,
        .pipeline_cache_save = flux_pipeline_cache_file_save,
        .pipeline_cache_userdata = &cache,
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

    /* ===== end shared bootstrap (window + device + surface) =====
     * Everything below is what this example is actually about: the
     * canvas, its resources, and the per-frame draw calls. */
    flux_canvas_desc cdesc = {
        .type = FLUX_TYPE_CANVAS_DESC,
        .surface = surface,
    };
    flux_canvas *canvas = nullptr;
    flux_result r = flux_canvas_create(&cdesc, &canvas);
    if (r != FLUX_OK) {
        flux_error_info ei;
        flux_get_last_error(&ei);
        fprintf(stderr, "flux_canvas_create -> %s\n  %s\n", flux_result_string(r),
                ei.message ? ei.message : "(no info)");
        flux_surface_release(surface);
        vkDestroySurfaceKHR(flux_device_vk_instance(device), vk_surface, nullptr);
        flux_device_release(device);
        glfwDestroyWindow(win);
        glfwTerminate();
        return (int)r;
    }
    printf("canvas ready\n");

    /* Arena for per-frame path allocations. */
    flux_arena arena;
    if (flux_arena_init(&arena, 64 * 1024, nullptr) != FLUX_OK) {
        flux_canvas_destroy(canvas);
        flux_surface_release(surface);
        vkDestroySurfaceKHR(flux_device_vk_instance(device), vk_surface, nullptr);
        flux_device_release(device);
        glfwDestroyWindow(win);
        glfwTerminate();
        return 1;
    }

    /* Procedurally generated 64x64 magenta/cyan checker, then a
     * diagonal stripe overlay — enough to confirm filtering +
     * sampling + UV mapping look right. */
    enum { TEX_W = 64, TEX_H = 64 };
    uint32_t pixels[TEX_W * TEX_H];
    for (int yy = 0; yy < TEX_H; ++yy) {
        for (int xx = 0; xx < TEX_W; ++xx) {
            bool checker = ((xx / 8) ^ (yy / 8)) & 1;
            uint32_t a = 0xFF, r, g, b;
            if (checker) {
                r = 0xFF;
                g = 0x40;
                b = 0xCC;
            } else {
                r = 0x40;
                g = 0xCC;
                b = 0xFF;
            }
            /* Diagonal stripe */
            if (((xx + yy) % 16) < 2) {
                r = g = b = 0xFF;
            }
            pixels[yy * TEX_W + xx] =
                (a << 24) | (b << 16) | (g << 8) | r; /* RGBA8 little-endian */
        }
    }
    flux_image_desc idesc = {
        .type = FLUX_TYPE_IMAGE_DESC,
        .width = TEX_W,
        .height = TEX_H,
        .format = FLUX_FORMAT_RGBA8_UNORM,
        .initial_data = pixels,
    };
    flux_image *texture = nullptr;
    if (flux_image_create(device, &idesc, &texture) != FLUX_OK) {
        flux_error_info ei;
        flux_get_last_error(&ei);
        fprintf(stderr, "image_create failed: %s\n", ei.message ? ei.message : "?");
    } else {
        printf("texture ready (%dx%d)\n", TEX_W, TEX_H);
    }

    int frame_no = 0;
    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();

        flux_frame *frame = nullptr;
        r = flux_surface_begin_frame(surface, nullptr, &frame);
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

        flux_color clear = flux_color_rgba(20, 20, 28, 255);
        r = flux_canvas_begin_frame(canvas, frame, &clear);
        if (r != FLUX_OK)
            break;

        flux_surface_info info;
        flux_surface_get_info(surface, &info);
        float W = (float)info.width;
        float H = (float)info.height;
        float t = (float)glfwGetTime();

        /* Backdrop tile pattern. */
        for (int gy = 0; gy < 8; ++gy) {
            for (int gx = 0; gx < 14; ++gx) {
                float cell = W / 14.0f;
                uint8_t v = (uint8_t)(40 + 20 * ((gx + gy) & 1));
                flux_canvas_fill_rect_color(canvas,
                                            (flux_rect){gx * cell, gy * (H / 8.0f), cell, H / 8.0f},
                                            flux_color_rgba_premul(v, v, v + 8, 255));
            }
        }

        /* Three solid rectangles with translucent SRC_OVER. */
        flux_canvas_fill_rect_color(canvas, (flux_rect){80, 80, 280, 180},
                                    flux_color_rgba_premul(240, 80, 80, 200));
        flux_canvas_fill_rect_color(canvas, (flux_rect){200, 160, 280, 180},
                                    flux_color_rgba_premul(80, 200, 90, 200));
        flux_canvas_fill_rect_color(canvas, (flux_rect){320, 240, 280, 180},
                                    flux_color_rgba_premul(80, 130, 240, 200));

        /* Linear gradient over a rect. */
        {
            flux_gradient_stop stops[3] = {
                {0.0f, flux_color_rgba_premul(255, 80, 120, 255)},
                {0.5f, flux_color_rgba_premul(255, 220, 80, 255)},
                {1.0f, flux_color_rgba_premul(60, 200, 255, 255)},
            };
            flux_paint g =
                flux_paint_linear_gradient((flux_point){640, 80}, (flux_point){920, 280}, stops, 3);
            flux_canvas_fill_rect(canvas, (flux_rect){640, 80, 280, 200}, &g);
        }

        /* Radial gradient over a circle path. */
        {
            float rcx = W * 0.85f, rcy = H * 0.78f;
            flux_path *rcirc = nullptr;
            (void)flux_path_create(&rcirc, &arena);
            if (rcirc) {
                flux_path_add_circle(rcirc, rcx, rcy, 90.0f);
                flux_gradient_stop stops[3] = {
                    {0.0f, flux_color_rgba_premul(255, 255, 255, 255)},
                    {0.7f, flux_color_rgba_premul(180, 100, 255, 240)},
                    {1.0f, flux_color_rgba_premul(30, 10, 80, 200)},
                };
                flux_paint g = flux_paint_radial_gradient((flux_point){rcx, rcy}, 90.0f, stops, 3);
                flux_canvas_fill_path(canvas, rcirc, &g);
            }
        }

        /* Rounded rect via fill_path (cubics flattened by the canvas). */
        flux_path *rrect = nullptr;
        (void)flux_path_create(&rrect, &arena);
        if (rrect) {
            flux_path_add_round_rect(rrect, (flux_rect){60, 360, 380, 140}, 32.0f);
            flux_paint p = flux_paint_default();
            p.color = flux_color_rgba_premul(255, 220, 120, 230);
            flux_canvas_fill_path(canvas, rrect, &p);
        }

        /* Animated circle. */
        flux_path *circ = nullptr;
        (void)flux_path_create(&circ, &arena);
        if (circ) {
            float cx = W * 0.75f + 40.0f * cosf(t);
            float cy = H * 0.55f + 40.0f * sinf(t * 1.3f);
            flux_path_add_circle(circ, cx, cy, 90.0f);
            flux_paint p = flux_paint_default();
            p.color = flux_color_rgba_premul(220, 140, 240, 220);
            flux_canvas_fill_path(canvas, circ, &p);
        }

        /* Concave 5-point star — exercises the tessellator. */
        flux_path *star = nullptr;
        (void)flux_path_create(&star, &arena);
        if (star) {
            float cx = W * 0.5f, cy = H * 0.78f;
            float r_out = 64.0f, r_in = 26.0f;
            for (int k = 0; k < 10; ++k) {
                float a = (float)k * 3.14159265359f / 5.0f - 1.5707963f;
                float r = (k & 1) ? r_in : r_out;
                float x = cx + r * cosf(a);
                float y = cy + r * sinf(a);
                if (k == 0)
                    flux_path_move_to(star, x, y);
                else
                    flux_path_line_to(star, x, y);
            }
            flux_path_close(star);
            flux_paint p = flux_paint_default();
            p.color = flux_color_rgba_premul(255, 215, 80, 240);
            flux_canvas_fill_path(canvas, star, &p);
        }

        /* Rotated rectangle via the state stack. */
        flux_canvas_save(canvas);
        flux_canvas_translate(canvas, W * 0.5f, H * 0.5f);
        flux_canvas_rotate(canvas, t * 0.7f);
        flux_canvas_fill_rect_color(canvas, (flux_rect){-40, -40, 80, 80},
                                    flux_color_rgba_premul(255, 240, 240, 255));
        flux_canvas_restore(canvas);

        /* Two image draws of the same texture, different sizes. */
        if (texture) {
            flux_canvas_draw_image(canvas, texture, (flux_rect){60, 540, 128, 128}, nullptr);
            flux_canvas_draw_image(canvas, texture, (flux_rect){220, 540, 256, 128}, nullptr);
        }

        /* Stroked zig-zag with miter joins + round caps. */
        flux_path *stroked = nullptr;
        (void)flux_path_create(&stroked, &arena);
        if (stroked) {
            flux_path_move_to(stroked, 80, 60);
            flux_path_line_to(stroked, 160, 30);
            flux_path_line_to(stroked, 240, 60);
            flux_path_line_to(stroked, 320, 30);
            flux_path_line_to(stroked, 400, 60);
            flux_paint sp = flux_paint_default();
            sp.color = flux_color_rgba_premul(255, 255, 255, 255);
            sp.stroke_width = 6.0f;
            sp.join = FLUX_JOIN_MITER;
            sp.cap = FLUX_CAP_ROUND;
            sp.miter_limit = 4.0f;
            flux_canvas_stroke_path(canvas, stroked, &sp);
        }

        /* Stroked closed triangle with round joins. */
        flux_path *stri = nullptr;
        (void)flux_path_create(&stri, &arena);
        if (stri) {
            float baseX = W - 220.0f;
            float baseY = 80.0f;
            flux_path_move_to(stri, baseX, baseY + 120);
            flux_path_line_to(stri, baseX + 70, baseY);
            flux_path_line_to(stri, baseX + 140, baseY + 120);
            flux_path_close(stri);
            flux_paint sp = flux_paint_default();
            sp.color = flux_color_rgba_premul(180, 255, 200, 255);
            sp.stroke_width = 8.0f;
            sp.join = FLUX_JOIN_ROUND;
            flux_canvas_stroke_path(canvas, stri, &sp);
        }

        flux_arena_reset(&arena);
        flux_canvas_end_frame(canvas);

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

        if (++frame_no == 1)
            printf("first canvas frame presented (extent %ux%u)\n", info.width, info.height);
    }

    flux_device_wait_idle(device);
    if (texture)
        flux_image_release(texture);
    flux_canvas_destroy(canvas);
    flux_arena_destroy(&arena);
    flux_surface_release(surface);
    vkDestroySurfaceKHR(flux_device_vk_instance(device), vk_surface, nullptr);
    flux_device_release(device);
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
