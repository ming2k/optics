/*
 * liquid_glass — analytic thick liquid glass over a chaotic backdrop.
 *
 * A demo of REAL backdrop blur: the glass panel blurs whatever the canvas
 * has actually rendered behind it, not a static stand-in texture. This
 * uses the canvas render-target capture seam (ADR-0017):
 *
 *   1. CAPTURE  — render the chaotic scene into a flux_image via
 *      flux_canvas_begin_target / end_target.
 *   2. MATERIAL — create fixed-cost frost, then feed sharp + frosted images
 *      to prism_liquid_glass_filter_apply (the prism material library). Its
 *      analytic SDF drives refraction, chromatic dispersion, rim lighting
 *      and exact rounded alpha.
 *   3. COMPOSITE — draw the sharp capture and the transparent glass output.
 *
 * Key flux APIs:  flux_image_create_render_target, flux_canvas_begin_target,
 *                 flux_canvas_end_target, flux_blur_filter_apply,
 *                 flux_canvas_draw_image
 * Key prism APIs: prism_liquid_glass_filter_apply
 *
 * Plumbing (raw Vulkan, not flux): GLFW window + VkSurfaceKHR creation.
 * Requires -Deffect=true -Dprism=true.
 */
#include <flux/effect.h>
#include <flux/flux.h>
#include <flux/vulkan.h>
#include <prism/prism.h>

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
        (void)flux_path_create(&p, arena);
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
        (void)flux_path_create(&p, arena);
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
    flux_blur_filter *blur_filter = nullptr;
    prism_liquid_glass_filter *glass_filter = nullptr;
    if (flux_blur_filter_create(device, &blur_filter) != FLUX_OK ||
        prism_liquid_glass_filter_create(device, &glass_filter) != FLUX_OK) {
        fprintf(stderr, "liquid-glass filters create failed\n");
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
            (void)flux_image_create(device, &ndesc, &noise_img);
            free(noise_data);
        }
    }

    int frame_no = 0;
    double previous_time = glfwGetTime();
    float droplet_position = 0.0f;
    float droplet_velocity = 0.0f;
    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();

        double now = glfwGetTime();
        float dt = fminf(fmaxf((float)(now - previous_time), 0.0f), 1.0f / 30.0f);
        previous_time = now;
        float droplet_target = fmodf((float)now, 5.0f) < 2.5f ? 0.0f : 1.0f;
        /* Semi-implicit damped spring. Each target change overshoots before
         * settling, while the two SDFs form and release a continuous neck. */
        const float spring_stiffness = 72.0f;
        const float spring_damping = 11.0f;
        float spring_force = (droplet_target - droplet_position) * spring_stiffness -
                             droplet_velocity * spring_damping;
        droplet_velocity += spring_force * dt;
        droplet_position += droplet_velocity * dt;

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

        /* Toggle body plus a small spring-driven droplet. Their rounded SDFs
         * are smoothly unioned by the liquid-glass pass. */
        float gw = 340.0f;
        float gh = 100.0f;
        float gx = W * 0.5f - gw * 0.5f;
        float gy = H * 0.5f - gh * 0.5f;
        float gr = gh * 0.5f;
        float cx = gx + gw * 0.5f;
        float cy = gy + gh * 0.5f;

        /* ===== STEP 2: FROST + ANALYTIC GLASS (no active pass) ===== */
        flux_image *blurred = nullptr;
        flux_effect_blur_desc bd = FLUX_EFFECT_BLUR_DESC_INIT;
        bd.input = capture;
        bd.sigma = 12.0f;
        r = flux_blur_filter_apply(blur_filter, frame, &bd, &blurred);
        if (r != FLUX_OK) {
            fprintf(stderr, "blur: %s\n", flux_result_string(r));
            break;
        }
        prism_liquid_glass_group body = PRISM_LIQUID_GLASS_GROUP_INIT;
        body.shapes[0] =
            (prism_liquid_glass_shape){.bounds = {gx, gy, gw, gh}, .corner_radius = gr};
        body.shapes[1] = (prism_liquid_glass_shape){
            .bounds = {gx + gw - 22.0f + droplet_position * 84.0f, gy + 11.0f, 78.0f, 78.0f},
            .corner_radius = 39.0f,
        };
        body.shape_count = 2;
        body.blend_radius = 24.0f;
        body.opacity = 1.0f;
        body.shadow_alpha = 0.20f;
        body.shadow_blur = 12.0f;
        body.shadow_offset_y = 6.0f;
        body.tint_color = 0xFFFFFFu;
        prism_liquid_glass_desc gd = PRISM_LIQUID_GLASS_DESC_INIT;
        gd.input = capture;
        gd.blurred_input = blurred;
        gd.groups = &body;
        gd.group_count = 1;
        gd.refraction = 13.0f;
        gd.chromatic_aberration = 1.6f;
        gd.edge_width = 22.0f;
        flux_image *glass_output = nullptr;
        r = prism_liquid_glass_filter_apply(glass_filter, frame, &gd, &glass_output);
        if (r != FLUX_OK) {
            fprintf(stderr, "liquid glass: %s\n", flux_result_string(r));
            break;
        }

        /* ===== STEP 3: COMPOSITE onto the frame ===== */
        flux_color clear = flux_color_rgba(10, 10, 14, 255);
        r = flux_canvas_begin(canvas, frame, &clear);
        if (r != FLUX_OK)
            break;

        /* Sharp backdrop: draw the captured scene. */
        flux_canvas_draw_image(canvas, capture, (flux_rect){0, 0, W, H}, nullptr);

        /* The glass pass casts the body's SDF drop shadow itself. */
        flux_canvas_draw_image(canvas, glass_output, (flux_rect){0, 0, W, H}, nullptr);

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
        (void)flux_path_create(&p_sun, &arena);
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
        (void)flux_path_create(&p_moon, &arena);
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
        (void)flux_path_create(&p_sunrise, &arena);
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
    prism_liquid_glass_filter_release(glass_filter);
    flux_blur_filter_release(blur_filter);
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
