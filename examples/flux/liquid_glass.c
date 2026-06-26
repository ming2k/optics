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
static void draw_chaos(flux_canvas *c, flux_arena *arena, float W, float H, float t) {
    /* Base gradient wash. */
    {
        flux_gradient_stop stops[3] = {
            {0.0f, flux_color_rgba_premul(30, 10, 50, 255)},
            {0.5f, flux_color_rgba_premul(10, 30, 60, 255)},
            {1.0f, flux_color_rgba_premul(50, 15, 40, 255)},
        };
        flux_paint g = flux_paint_linear_gradient((flux_point){0, 0}, (flux_point){W, H}, stops, 3);
        flux_canvas_fill_rect(c, (flux_rect){0, 0, W, H}, &g);
    }

    /* Travelling saturated blobs — high-frequency detail so the blur reads. */
    for (int i = 0; i < 5; ++i) {
        float ph = (float)i * 1.7f;
        float bx = W * (0.15f + 0.7f * (0.5f + 0.5f * sinf(t * 0.6f + ph)));
        float by = H * (0.15f + 0.7f * (0.5f + 0.5f * cosf(t * 0.45f + ph * 1.3f)));
        float br = 60.0f + 30.0f * sinf(t * 1.1f + ph);
        flux_path *blob = nullptr;
        (void)flux_path_create(&blob, arena);
        if (!blob)
            continue;
        flux_path_add_circle(blob, bx, by, br);
        float hue = fmodf(ph * 0.27f, 1.0f);
        uint8_t cr = (uint8_t)(255.0f * (0.5f + 0.5f * sinf(hue * 6.28f)));
        uint8_t cg = (uint8_t)(255.0f * (0.5f + 0.5f * sinf(hue * 6.28f + 2.1f)));
        uint8_t cb = (uint8_t)(255.0f * (0.5f + 0.5f * sinf(hue * 6.28f + 4.2f)));
        flux_paint p = flux_paint_radial_gradient(
            (flux_point){bx, by}, br,
            (flux_gradient_stop[2]){{0.0f, flux_color_rgba_premul(cr, cg, cb, 235)},
                                    {1.0f, flux_color_rgba_premul(cr, cg, cb, 0)}},
            2);
        flux_canvas_fill_path(c, blob, &p);
    }

    /* Diagonal stripes for extra high-frequency contrast. */
    {
        flux_path *str = nullptr;
        (void)flux_path_create(&str, arena);
        if (str) {
            flux_path_add_rect(str, (flux_rect){0, 0, W, H});
            flux_gradient_stop stops[6] = {
                {0.00f, flux_color_rgba_premul(255, 255, 255, 18)},
                {0.16f, flux_color_rgba_premul(255, 255, 255, 0)},
                {0.33f, flux_color_rgba_premul(255, 255, 255, 18)},
                {0.50f, flux_color_rgba_premul(255, 255, 255, 0)},
                {0.66f, flux_color_rgba_premul(255, 255, 255, 18)},
                {0.83f, flux_color_rgba_premul(255, 255, 255, 0)},
            };
            flux_paint g = flux_paint_linear_gradient((flux_point){0, 0},
                                                      (flux_point){W + H, W + H}, stops, 6);
            flux_canvas_fill_path(c, str, &g);
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
            draw_chaos(canvas, &arena, W, H, t);
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

        /* Glass panel geometry, gently orbiting the centre. */
        float gw = fminf(W, H) * 0.46f;
        float gh = gw * 0.62f;
        float gx = W * 0.5f - gw * 0.5f + 70.0f * cosf(t * 0.4f);
        float gy = H * 0.5f - gh * 0.5f + 40.0f * sinf(t * 0.6f);
        float gr = gh * 0.30f;
        flux_rect glass = {gx, gy, gw, gh};
        float cx = gx + gw * 0.5f;
        float cy = gy + gh * 0.5f;

        /* Drop shadow. */
        {
            flux_path *sh = nullptr;
            (void)flux_path_create(&sh, &arena);
            if (sh) {
                flux_path_add_round_rect(sh, (flux_rect){gx + 6.0f, gy + 12.0f, gw, gh}, gr);
                flux_paint p = flux_paint_default();
                p.color = flux_color_rgba_premul(0, 0, 0, 90);
                flux_canvas_fill_path(canvas, sh, &p);
            }
        }

        /* Blurred backdrop, scoped to the glass region (clip_rect replaces
         * the scissor, so we scope with save/restore). */
        flux_canvas_save(canvas);
        flux_canvas_clip_rect(canvas, glass);
        if (blurred) {
            flux_canvas_draw_image(canvas, blurred, (flux_rect){0, 0, W, H}, nullptr);
        }
        flux_canvas_restore(canvas);

        /* Build the rounded glass shape once, reuse for all gradient fills. */
        flux_path *shape = nullptr;
        (void)flux_path_create(&shape, &arena);
        if (shape)
            flux_path_add_round_rect(shape, glass, gr);

        /* L1 VOLUME / THICKNESS: cool frosted centre, thinner rim. */
        if (shape) {
            flux_gradient_stop stops[4] = {
                {0.00f, flux_color_rgba_premul(115, 130, 150, 205)},
                {0.50f, flux_color_rgba_premul(95, 110, 132, 175)},
                {0.85f, flux_color_rgba_premul(78, 92, 116, 130)},
                {1.00f, flux_color_rgba_premul(60, 74, 98, 80)},
            };
            float rad = fmaxf(gw, gh) * 0.62f;
            flux_paint vol = flux_paint_radial_gradient((flux_point){cx, cy}, rad, stops, 4);
            flux_canvas_fill_path(canvas, shape, &vol);
        }

        /* L2 FRESNEL EDGE: bright glossy rim. */
        if (shape) {
            flux_gradient_stop stops[4] = {
                {0.50f, flux_color_rgba_premul(0, 0, 0, 0)},
                {0.78f, flux_color_rgba_premul(255, 255, 255, 40)},
                {0.93f, flux_color_rgba_premul(255, 255, 255, 160)},
                {1.00f, flux_color_rgba_premul(255, 255, 255, 235)},
            };
            float rad = fmaxf(gw, gh) * 0.55f;
            flux_paint fres = flux_paint_radial_gradient((flux_point){cx, cy}, rad, stops, 4);
            flux_canvas_fill_path(canvas, shape, &fres);
        }

        /* L3 SPECULAR SHEEN: bright band across the top. */
        if (shape) {
            flux_gradient_stop stops[3] = {
                {0.00f, flux_color_rgba_premul(255, 255, 255, 110)},
                {0.45f, flux_color_rgba_premul(255, 255, 255, 30)},
                {1.00f, flux_color_rgba_premul(0, 0, 0, 0)},
            };
            flux_paint sheen = flux_paint_linear_gradient(
                (flux_point){gx, gy}, (flux_point){gx, gy + gh * 0.55f}, stops, 3);
            flux_canvas_fill_path(canvas, shape, &sheen);
        }

        /* L4/L5 edge dispersion hint + crisp hairline. */
        {
            flux_path *rim = nullptr;
            (void)flux_path_create(&rim, &arena);
            if (rim) {
                flux_path_add_round_rect(
                    rim, (flux_rect){gx + 1.0f, gy + 1.0f, gw - 2.0f, gh - 2.0f}, gr - 1.0f);
                flux_paint sp = flux_paint_default();
                sp.color = flux_color_rgba_premul(255, 235, 210, 55);
                sp.stroke_width = 1.0f;
                sp.join = FLUX_JOIN_ROUND;
                sp.cap = FLUX_CAP_ROUND;
                flux_canvas_stroke_path(canvas, rim, &sp);
            }
            flux_path *hair = nullptr;
            (void)flux_path_create(&hair, &arena);
            if (hair) {
                flux_path_add_round_rect(hair, glass, gr);
                flux_paint sp = flux_paint_default();
                sp.color = flux_color_rgba_premul(255, 255, 255, 150);
                sp.stroke_width = 1.0f;
                sp.join = FLUX_JOIN_ROUND;
                flux_canvas_stroke_path(canvas, hair, &sp);
            }
        }

        flux_arena_reset(&arena);
        flux_canvas_end(canvas);
        flux_effect_reset(device);

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
