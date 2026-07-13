/*
 * liquid_glass — Apple-style translucent glass over a chaotic backdrop.
 *
 * A demo of REAL backdrop blur: the glass panel blurs whatever the canvas
 * has actually rendered behind it, not a static stand-in texture. This
 * uses the canvas render-target capture seam (ADR-0017):
 *
 *   1. CAPTURE  — render the chaotic scene into a flux_image via
 *      flux_canvas_begin_target / end_target.
 *   2. EFFECT   — blur the captured image with flux_effect_blur.
 *   3. COMPOSITE — draw the sharp capture as the backdrop, then draw the
 *      blurred capture behind the glass (clipped to its rounded shape),
 *      then the glass layers (fresnel rim, volume frost, specular sheen).
 *
 * The glass itself is built from three physical principles, each a gradient
 * layer clipped to the rounded shape (flux_path_add_round_rect + gradient
 * fills, which evaluate per-fragment in screen space):
 *
 *   1. FRESNEL EDGE  — reflectance rises where the view grazes the surface
 *      (the rim). A radial gradient, transparent at the centre and bright
 *      white at the perimeter, produces the hard glossy shell.
 *   2. VOLUME / THICKNESS — the interior reads as a fluid mass: more frosted
 *      where thick (centre), clearer where thin (toward the rim). A radial
 *      gradient drives the frost; a cool blue-white tint reads as glass.
 *   3. SMOOTH TRANSITION — every layer's coverage comes from a rounded-rect
 *      path and its intensity from a continuous radial field, so the
 *      gradient rolls smoothly edge→interior with no seam.
 *
 * Key flux APIs:  flux_image_create_render_target, flux_canvas_begin_target,
 *                 flux_canvas_end_target, flux_effect_blur, flux_effect_reset,
 *                 flux_canvas_draw_image, flux_canvas_clip_rect,
 *                 flux_path_add_round_rect, flux_canvas_fill_path,
 *                 flux_paint_radial_gradient, flux_paint_linear_gradient
 *
 * Plumbing (raw Vulkan, not flux): GLFW window + VkSurfaceKHR creation.
 * Requires -Deffect=true.
 */
#include <flux/effect.h>
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

/* Draw the chaotic scene into whatever canvas pass is currently active
 * (used both for the sharp backdrop and — via the capture target — for
 * the blurred glass content). `t` animates the blobs. */
static void draw_chaos(flux_canvas *c, flux_arena *arena, float W, float H, flux_image *noise_img) {
    /* Base warm gradient wash. */
    {
        flux_gradient_stop stops[2] = {
            {0.0f, flux_color_rgba_premul(235, 205, 175, 255)}, // top-leftish warm beige
            {1.0f, flux_color_rgba_premul(110, 60, 100, 255)},  // bottom-rightish dark magenta
        };
        flux_paint g = flux_paint_linear_gradient((flux_point){0, 0}, (flux_point){W, H}, stops, 2);
        flux_canvas_fill_rect(c, (flux_rect){0, 0, W, H}, &g);
    }

    /* Top right orange/red radial */
    {
        flux_gradient_stop stops[2] = {
            {0.0f, flux_color_rgba_premul(210, 80, 50, 200)},
            {1.0f, flux_color_rgba_premul(210, 80, 50, 0)},
        };
        flux_paint g =
            flux_paint_radial_gradient((flux_point){W * 0.85f, H * 0.15f}, W * 0.6f, stops, 2);
        flux_path *p = nullptr;
        flux_path_create(&p, arena);
        if (p) {
            flux_path_add_rect(p, (flux_rect){0, 0, W, H});
            flux_canvas_fill_path(c, p, &g);
        }
    }

    /* Bottom left purple/blue radial */
    {
        flux_gradient_stop stops[2] = {
            {0.0f, flux_color_rgba_premul(80, 70, 140, 200)},
            {1.0f, flux_color_rgba_premul(80, 70, 140, 0)},
        };
        flux_paint g =
            flux_paint_radial_gradient((flux_point){W * 0.15f, H * 0.85f}, W * 0.6f, stops, 2);
        flux_path *p = nullptr;
        flux_path_create(&p, arena);
        if (p) {
            flux_path_add_rect(p, (flux_rect){0, 0, W, H});
            flux_canvas_fill_path(c, p, &g);
        }
    }

    /* Noise overlay */
    if (noise_img) {
        for (float y = 0; y < H; y += 256.0f) {
            for (float x = 0; x < W; x += 256.0f) {
                flux_canvas_draw_image(c, noise_img, (flux_rect){x, y, 256.0f, 256.0f}, nullptr);
            }
        }
    }
}

int main(void) {
    if (!glfwInit() || !glfwVulkanSupported()) {
        fprintf(stderr, "glfw vk init failed\n");
        return 1;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    GLFWwindow *win = glfwCreateWindow(960, 600, "flux liquid glass", nullptr, nullptr);
    if (!win) {
        glfwTerminate();
        return 1;
    }

    uint32_t ext_count = 0;
    const char **req_exts = glfwGetRequiredInstanceExtensions(&ext_count);
    const char *device_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    flux_pipeline_cache_file cache = FLUX_PIPELINE_CACHE_FILE_INIT;
    flux_pipeline_cache_file_set_default_path(&cache, "liquid_glass.bin");

    flux_device_desc ddesc = {
        .type = FLUX_TYPE_DEVICE_DESC,
        .log = flux_console_logger,
        .validation = FLUX_VALIDATION_AUTO,
        .required_instance_extensions = req_exts,
        .required_instance_extension_count = ext_count,
        .required_device_extensions = device_exts,
        .required_device_extension_count = sizeof(device_exts) / sizeof(*device_exts),
        .frames_in_flight = 1,
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

    /* ===== end shared bootstrap (window + device + surface) ===== */

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

    flux_arena arena;
    if (flux_arena_init(&arena, 128 * 1024, nullptr) != FLUX_OK) {
        flux_canvas_destroy(canvas);
        flux_surface_release(surface);
        vkDestroySurfaceKHR(flux_device_vk_instance(device), vk_surface, nullptr);
        flux_device_release(device);
        glfwDestroyWindow(win);
        glfwTerminate();
        return 1;
    }

    /* The capture target: a render-target image matching the surface format
     * and extent. The chaotic scene is rendered into it each frame, then
     * blurred and composited back. This is the real backdrop-blur path —
     * the glass blurs the actual scene, not a stand-in texture. */
    VkFormat sfmt = flux_surface_vk_format(surface);
    flux_format target_fmt =
        (sfmt == VK_FORMAT_B8G8R8A8_UNORM) ? FLUX_FORMAT_BGRA8_UNORM : FLUX_FORMAT_RGBA8_UNORM;
    flux_surface_info info;
    flux_surface_get_info(surface, &info);
    flux_image *capture = nullptr;
    if (flux_image_create_render_target(device, info.width, info.height, target_fmt, &capture) !=
        FLUX_OK) {
        fprintf(stderr, "render-target create failed\n");
        return 1;
    }
    /* Create static noise texture for grain. */
    flux_image *noise_img = nullptr;
    {
        uint8_t *noise_data = (uint8_t *)malloc(256 * 256 * 4);
        if (noise_data) {
            for (int i = 0; i < 256 * 256 * 4; i += 4) {
                uint8_t v = rand() % 256;
                uint8_t a = 10 + (rand() % 15);
                noise_data[i] = (uint8_t)((v * a) / 255);
                noise_data[i + 1] = (uint8_t)((v * a) / 255);
                noise_data[i + 2] = (uint8_t)((v * a) / 255);
                noise_data[i + 3] = a;
            }
            flux_image_desc ndesc = FLUX_IMAGE_DESC_INIT;
            ndesc.width = 256;
            ndesc.height = 256;
            ndesc.format = FLUX_FORMAT_RGBA8_UNORM;
            ndesc.initial_data = noise_data;
            flux_image_create(device, &ndesc, &noise_img);
            free(noise_data);
        }
    }

    int frame_no = 0;
    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();

        /* Effect leases are device-wide and may be referenced by either frame
         * slot. A single begin_frame fence is not a global quiescent point. */
        flux_device_wait_idle(device);
        flux_effect_reset(device);

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

        flux_surface_get_info(surface, &info);
        float W = (float)info.width;
        float H = (float)info.height;
        float t = (float)glfwGetTime();

        /* ===== STEP 1: CAPTURE the chaotic scene into `capture` ===== */
        {
            flux_color clear = flux_color_rgba(18, 14, 28, 255);
            r = flux_canvas_begin_target(canvas, frame, capture, &clear);
            if (r != FLUX_OK) {
                fprintf(stderr, "begin_target: %s\n", flux_result_string(r));
                break;
            }
            draw_chaos(canvas, &arena, W, H, noise_img);
            flux_arena_reset(&arena);
            flux_canvas_end_target(canvas);
        }

        /* ===== STEP 2: BLUR the captured scene (compute, no active pass) ===== */
        flux_image *blurred = nullptr;
        flux_effect_blur_desc bd = FLUX_EFFECT_BLUR_DESC_INIT;
        bd.input = capture;
        /* sigma kept in the tested regime (see test_canvas_target).
         * sigma=18 here previously made each frame blow past the 2 s
         * frame fence on weak / software Vulkan, saturating the shared
         * display device and freezing the compositor. */
        bd.sigma = 6.0f;
        VkCommandBuffer cmd = flux_frame_vk_command_buffer(frame);
        r = flux_effect_blur(cmd, &bd, &blurred);
        if (r != FLUX_OK) {
            fprintf(stderr, "blur: %s\n", flux_result_string(r));
            break;
        }

        /* ===== STEP 3: COMPOSITE onto the frame ===== */
        flux_color clear = flux_color_rgba(10, 10, 14, 255);
        r = flux_canvas_begin(canvas, frame, &clear);
        if (r != FLUX_OK)
            break;

        /* Sharp backdrop: draw the captured scene. */
        flux_canvas_draw_image(canvas, capture, (flux_rect){0, 0, W, H}, nullptr);

        /* Toggle switch (pill shape) centered. */
        float gw = 340.0f;
        float gh = 100.0f;
        float gx = W * 0.5f - gw * 0.5f;
        float gy = H * 0.5f - gh * 0.5f;
        float gr = gh * 0.5f;
        flux_rect glass = {gx, gy, gw, gh};
        float cx = gx + gw * 0.5f;
        float cy = gy + gh * 0.5f;

        /* Drop shadow. */
        {
            flux_path *sh = nullptr;
            (void)flux_path_create(&sh, &arena);
            if (sh) {
                flux_path_add_round_rect(sh, (flux_rect){gx + 4.0f, gy + 8.0f, gw, gh}, gr);
                flux_paint p = flux_paint_default();
                p.color = flux_color_rgba_premul(0, 0, 0, 40);
                flux_canvas_fill_path(canvas, sh, &p);
            }
        }

        /* Blurred backdrop, scoped to the glass region. */
        flux_canvas_save(canvas);
        flux_canvas_clip_rect(canvas, glass);
        if (blurred) {
            flux_canvas_draw_image(canvas, blurred, (flux_rect){0, 0, W, H}, nullptr);
        }

        /* Build the rounded glass shape once, reuse. */
        flux_path *shape = nullptr;
        (void)flux_path_create(&shape, &arena);
        if (shape)
            flux_path_add_round_rect(shape, glass, gr);

        /* Volume / Color tint. Warm pinkish-purple glass tint. */
        if (shape) {
            flux_gradient_stop stops[4] = {
                {0.00f, flux_color_rgba_premul(170, 110, 140, 160)},
                {0.50f, flux_color_rgba_premul(150, 90, 130, 130)},
                {0.85f, flux_color_rgba_premul(130, 80, 120, 100)},
                {1.00f, flux_color_rgba_premul(110, 70, 110, 70)},
            };
            flux_paint vol = flux_paint_radial_gradient((flux_point){cx, cy}, gw * 0.6f, stops, 4);
            flux_canvas_fill_path(canvas, shape, &vol);
        }

        /* Active thumb indicator (behind Sun icon). */
        {
            float thumb_w = 100.0f;
            float thumb_h = gh - 12.0f;
            flux_rect thumb = {gx + 6.0f, gy + 6.0f, thumb_w, thumb_h};
            flux_path *tp = nullptr;
            (void)flux_path_create(&tp, &arena);
            if (tp) {
                flux_path_add_round_rect(tp, thumb, thumb_h * 0.5f);
                flux_paint pt = flux_paint_default();
                pt.color = flux_color_rgba_premul(255, 230, 220, 140);
                flux_canvas_fill_path(canvas, tp, &pt);
            }
        }
        flux_canvas_restore(canvas);

        /* Fresnel Edge & Specular */
        if (shape) {
            flux_gradient_stop stops[4] = {
                {0.60f, flux_color_rgba_premul(0, 0, 0, 0)},
                {0.85f, flux_color_rgba_premul(255, 230, 240, 30)},
                {0.96f, flux_color_rgba_premul(255, 240, 250, 120)},
                {1.00f, flux_color_rgba_premul(255, 255, 255, 200)},
            };
            flux_paint fres =
                flux_paint_radial_gradient((flux_point){cx, cy}, gw * 0.55f, stops, 4);
            flux_canvas_fill_path(canvas, shape, &fres);

            flux_gradient_stop s_stops[3] = {
                {0.00f, flux_color_rgba_premul(255, 255, 255, 90)},
                {0.45f, flux_color_rgba_premul(255, 255, 255, 20)},
                {1.00f, flux_color_rgba_premul(0, 0, 0, 0)},
            };
            flux_paint sheen = flux_paint_linear_gradient(
                (flux_point){gx, gy}, (flux_point){gx, gy + gh * 0.45f}, s_stops, 3);
            flux_canvas_fill_path(canvas, shape, &sheen);
        }

        /* Border hairlines */
        {
            flux_path *hair = nullptr;
            (void)flux_path_create(&hair, &arena);
            if (hair) {
                flux_path_add_round_rect(hair, glass, gr);
                flux_paint sp = flux_paint_default();
                sp.color = flux_color_rgba_premul(255, 255, 255, 120);
                sp.stroke_width = 1.0f;
                sp.join = FLUX_JOIN_ROUND;
                flux_canvas_stroke_path(canvas, hair, &sp);
            }
        }

        /* Icons */
        flux_color icon_col = flux_color_rgba_premul(30, 20, 50, 220);
        float ix_sun = gx + gh * 0.5f + 6.0f;
        float ix_moon = cx;
        float ix_sunrise = gx + gw - gh * 0.5f - 6.0f;
        float iy = cy;

        flux_paint sp_icon = flux_paint_default();
        sp_icon.color = icon_col;
        sp_icon.stroke_width = 2.5f;
        sp_icon.cap = FLUX_CAP_ROUND;
        sp_icon.join = FLUX_JOIN_ROUND;

        /* Sun icon */
        flux_path *p_sun = nullptr;
        flux_path_create(&p_sun, &arena);
        if (p_sun) {
            flux_path_add_circle(p_sun, ix_sun, iy, 7.0f);
            for (int i = 0; i < 8; i++) {
                float a = i * 3.14159f / 4.0f;
                flux_path_move_to(p_sun, ix_sun + cosf(a) * 11.0f, iy + sinf(a) * 11.0f);
                flux_path_line_to(p_sun, ix_sun + cosf(a) * 15.0f, iy + sinf(a) * 15.0f);
            }
            flux_canvas_stroke_path(canvas, p_sun, &sp_icon);
        }

        /* Moon icon */
        flux_path *p_moon = nullptr;
        flux_path_create(&p_moon, &arena);
        if (p_moon) {
            float mr = 13.0f;
            flux_path_move_to(p_moon, ix_moon + mr * 0.3f, iy - mr);
            flux_path_cubic_to(p_moon, ix_moon - mr * 1.2f, iy - mr, ix_moon - mr * 1.2f, iy + mr,
                               ix_moon + mr * 0.3f, iy + mr);
            flux_path_cubic_to(p_moon, ix_moon - mr * 0.2f, iy + mr * 0.5f, ix_moon - mr * 0.2f,
                               iy - mr * 0.5f, ix_moon + mr * 0.3f, iy - mr);
            flux_canvas_stroke_path(canvas, p_moon, &sp_icon);
        }

        /* Sunrise icon */
        flux_path *p_sunrise = nullptr;
        flux_path_create(&p_sunrise, &arena);
        if (p_sunrise) {
            float r = 10.0f;
            float k = r * 0.55228f;
            float ry = iy + 3.0f;
            flux_path_move_to(p_sunrise, ix_sunrise - r, ry);
            flux_path_cubic_to(p_sunrise, ix_sunrise - r, ry - k, ix_sunrise - k, ry - r,
                               ix_sunrise, ry - r);
            flux_path_cubic_to(p_sunrise, ix_sunrise + k, ry - r, ix_sunrise + r, ry - k,
                               ix_sunrise + r, ry);
            flux_path_move_to(p_sunrise, ix_sunrise - r - 4.0f, ry + 4.0f);
            flux_path_line_to(p_sunrise, ix_sunrise + r + 4.0f, ry + 4.0f);
            flux_path_move_to(p_sunrise, ix_sunrise - r + 2.0f, ry + 9.0f);
            flux_path_line_to(p_sunrise, ix_sunrise + r - 2.0f, ry + 9.0f);

            flux_path_move_to(p_sunrise, ix_sunrise - 12.0f, ry - 12.0f);
            flux_path_line_to(p_sunrise, ix_sunrise - 16.0f, ry - 16.0f);
            flux_path_move_to(p_sunrise, ix_sunrise, ry - 14.0f);
            flux_path_line_to(p_sunrise, ix_sunrise, ry - 19.0f);
            flux_path_move_to(p_sunrise, ix_sunrise + 12.0f, ry - 12.0f);
            flux_path_line_to(p_sunrise, ix_sunrise + 16.0f, ry - 16.0f);

            flux_canvas_stroke_path(canvas, p_sunrise, &sp_icon);
        }

        flux_arena_reset(&arena);
        flux_canvas_end(canvas);

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
            printf("first glass frame presented (extent %ux%u)\n", info.width, info.height);
    }

    flux_device_wait_idle(device);
    if (noise_img)
        flux_image_release(noise_img);
    if (capture)
        flux_image_release(capture);
    flux_arena_destroy(&arena);
    flux_canvas_destroy(canvas);
    flux_surface_release(surface);
    vkDestroySurfaceKHR(flux_device_vk_instance(device), vk_surface, nullptr);
    flux_device_release(device);
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
