/*
 * image_animation — classic time-driven image animation on the 2D canvas.
 *
 * flux deliberately does not own an animation timeline. An application gets
 * time from its event loop, evaluates the animation, then records ordinary
 * image draws every frame. This example keeps that seam visible and combines
 * five small building blocks:
 *
 *   - easing + translation                    (bouncing card)
 *   - non-uniform scale                       (squash and stretch)
 *   - rotation + pulsing scale                (spinning card)
 *   - paint alpha                             (cross-fade)
 *   - normalised source rectangles            (sprite-sheet playback)
 *
 * All images are generated in memory so the example has no asset dependency.
 * Key flux APIs: flux_canvas_save/restore, translate/scale/rotate,
 *                flux_canvas_draw_image, flux_canvas_draw_image_sub.
 */
#include <flux/flux.h>
#include <flux/vulkan.h>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "pipeline_cache.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define PI 3.14159265358979323846f
#define CARD_SIZE 128
#define SPRITE_SIZE 64
#define SPRITE_FRAMES 8

static float clamp01(float x) {
    return fminf(fmaxf(x, 0.0f), 1.0f);
}

static float smoothstep01(float x) {
    x = clamp01(x);
    return x * x * (3.0f - 2.0f * x);
}

static uint32_t rgba8(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
}

static uint32_t rgba8_premul(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return rgba8((uint8_t)((uint32_t)r * a / 255u), (uint8_t)((uint32_t)g * a / 255u),
                 (uint8_t)((uint32_t)b * a / 255u), a);
}

/* Two opaque, deliberately asymmetric cards make rotation and cross-fading
 * easy to inspect visually. */
static void make_card_pixels(uint32_t pixels[CARD_SIZE * CARD_SIZE], int variant) {
    for (int y = 0; y < CARD_SIZE; ++y) {
        for (int x = 0; x < CARD_SIZE; ++x) {
            float u = (float)x / (float)(CARD_SIZE - 1);
            float v = (float)y / (float)(CARD_SIZE - 1);
            float dx = u - (variant ? 0.68f : 0.32f);
            float dy = v - (variant ? 0.35f : 0.65f);
            float glow = clamp01(1.0f - sqrtf(dx * dx + dy * dy) * 1.7f);
            float stripe = 0.5f + 0.5f * sinf((u * 5.0f + v * 3.0f) * PI);

            float r = variant ? 32.0f + 52.0f * stripe : 38.0f + 190.0f * glow;
            float g = variant ? 82.0f + 145.0f * glow : 42.0f + 82.0f * stripe;
            float b = variant ? 128.0f + 118.0f * stripe : 88.0f + 135.0f * (1.0f - u);

            /* Direction marker: a bright ring and a top-left corner flag. */
            float cx = u - 0.5f, cy = v - 0.5f;
            float ring = sqrtf(cx * cx + cy * cy);
            if (ring > 0.225f && ring < 0.29f) {
                r = variant ? 255.0f : 255.0f;
                g = variant ? 225.0f : 205.0f;
                b = variant ? 95.0f : 235.0f;
            }
            if (x >= 10 && x < 34 && y >= 10 && y < 25) {
                r = 255.0f;
                g = variant ? 120.0f : 238.0f;
                b = variant ? 96.0f : 150.0f;
            }

            pixels[y * CARD_SIZE + x] = rgba8((uint8_t)(clamp01(r / 255.0f) * 255.0f),
                                              (uint8_t)(clamp01(g / 255.0f) * 255.0f),
                                              (uint8_t)(clamp01(b / 255.0f) * 255.0f), 255);
        }
    }
}

static void over_pixel(uint32_t *dst, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    uint32_t d = *dst;
    uint8_t da = (uint8_t)(d >> 24);
    uint8_t dr = (uint8_t)d;
    uint8_t dg = (uint8_t)(d >> 8);
    uint8_t db = (uint8_t)(d >> 16);
    uint8_t sr = (uint8_t)((uint32_t)r * a / 255u);
    uint8_t sg = (uint8_t)((uint32_t)g * a / 255u);
    uint8_t sb = (uint8_t)((uint32_t)b * a / 255u);
    uint32_t inv = 255u - a;
    *dst =
        rgba8((uint8_t)(sr + (uint32_t)dr * inv / 255u), (uint8_t)(sg + (uint32_t)dg * inv / 255u),
              (uint8_t)(sb + (uint32_t)db * inv / 255u), (uint8_t)(a + (uint32_t)da * inv / 255u));
}

static void stamp_circle(uint32_t *cell, float cx, float cy, float radius, uint8_t r, uint8_t g,
                         uint8_t b, uint8_t opacity) {
    for (int y = 0; y < SPRITE_SIZE; ++y) {
        for (int x = 0; x < SPRITE_SIZE; ++x) {
            float dx = (float)x + 0.5f - cx;
            float dy = (float)y + 0.5f - cy;
            float coverage = clamp01(radius + 0.75f - sqrtf(dx * dx + dy * dy));
            if (coverage <= 0.0f)
                continue;
            uint8_t a = (uint8_t)(coverage * opacity);
            over_pixel(&cell[y * SPRITE_SIZE + x], r, g, b, a);
        }
    }
}

/* An eight-frame orbiting comet. Each frame occupies one horizontal cell. */
static void make_sprite_pixels(uint32_t pixels[SPRITE_FRAMES * SPRITE_SIZE * SPRITE_SIZE]) {
    for (int i = 0; i < SPRITE_FRAMES * SPRITE_SIZE * SPRITE_SIZE; ++i)
        pixels[i] = rgba8_premul(0, 0, 0, 0);

    for (int frame = 0; frame < SPRITE_FRAMES; ++frame) {
        uint32_t cell[SPRITE_SIZE * SPRITE_SIZE] = {0};
        float angle = 2.0f * PI * (float)frame / (float)SPRITE_FRAMES;
        stamp_circle(cell, 32.0f, 32.0f, 5.5f, 105, 145, 255, 180);
        for (int tail = 4; tail >= 1; --tail) {
            float a = angle - (float)tail * 0.16f;
            float x = 32.0f + 17.0f * cosf(a);
            float y = 32.0f + 17.0f * sinf(a);
            stamp_circle(cell, x, y, 3.0f + (float)(4 - tail) * 0.55f, 255, 86, 180,
                         (uint8_t)(36 + (4 - tail) * 28));
        }
        stamp_circle(cell, 32.0f + 17.0f * cosf(angle), 32.0f + 17.0f * sinf(angle), 7.0f, 255, 235,
                     125, 255);

        for (int y = 0; y < SPRITE_SIZE; ++y) {
            for (int x = 0; x < SPRITE_SIZE; ++x)
                pixels[y * (SPRITE_FRAMES * SPRITE_SIZE) + frame * SPRITE_SIZE + x] =
                    cell[y * SPRITE_SIZE + x];
        }
    }
}

static flux_image *make_image(flux_device *device, uint32_t width, uint32_t height,
                              const uint32_t *pixels) {
    flux_image_desc desc = FLUX_IMAGE_DESC_INIT;
    desc.width = width;
    desc.height = height;
    desc.format = FLUX_FORMAT_RGBA8_UNORM;
    desc.initial_data = pixels;
    flux_image *image = NULL;
    return flux_image_create(device, &desc, &image) == FLUX_OK ? image : NULL;
}

/* Tiny 5x7 labels keep this example independent from flux-text. */
static uint8_t glyph_row(char ch, int row) {
    static const uint8_t a[7] = {14, 17, 17, 31, 17, 17, 17};
    static const uint8_t b[7] = {30, 17, 17, 30, 17, 17, 30};
    static const uint8_t c[7] = {14, 17, 16, 16, 16, 17, 14};
    static const uint8_t d[7] = {30, 17, 17, 17, 17, 17, 30};
    static const uint8_t e[7] = {31, 16, 16, 30, 16, 16, 31};
    static const uint8_t f[7] = {31, 16, 16, 30, 16, 16, 16};
    static const uint8_t i[7] = {31, 4, 4, 4, 4, 4, 31};
    static const uint8_t n[7] = {17, 25, 25, 21, 19, 19, 17};
    static const uint8_t o[7] = {14, 17, 17, 17, 17, 17, 14};
    static const uint8_t p[7] = {30, 17, 17, 30, 16, 16, 16};
    static const uint8_t r[7] = {30, 17, 17, 30, 20, 18, 17};
    static const uint8_t s[7] = {15, 16, 16, 14, 1, 1, 30};
    static const uint8_t t[7] = {31, 4, 4, 4, 4, 4, 4};
    static const uint8_t u[7] = {17, 17, 17, 17, 17, 17, 14};
    const uint8_t *rows = NULL;
    switch (ch) {
    case 'A':
        rows = a;
        break;
    case 'B':
        rows = b;
        break;
    case 'C':
        rows = c;
        break;
    case 'D':
        rows = d;
        break;
    case 'E':
        rows = e;
        break;
    case 'F':
        rows = f;
        break;
    case 'I':
        rows = i;
        break;
    case 'N':
        rows = n;
        break;
    case 'O':
        rows = o;
        break;
    case 'P':
        rows = p;
        break;
    case 'R':
        rows = r;
        break;
    case 'S':
        rows = s;
        break;
    case 'T':
        rows = t;
        break;
    case 'U':
        rows = u;
        break;
    default:
        return 0;
    }
    return rows[row];
}

static void draw_label(flux_canvas *canvas, float x, float y, const char *text) {
    const float px = 2.4f;
    flux_color ink = flux_color_rgba_premul(185, 200, 225, 230);
    for (const char *p = text; *p; ++p, x += px * 6.0f) {
        for (int row = 0; row < 7; ++row) {
            uint8_t bits = glyph_row(*p, row);
            for (int col = 0; col < 5; ++col) {
                if (bits & (1u << (4 - col)))
                    flux_canvas_fill_rect_color(
                        canvas, (flux_rect){x + col * px, y + row * px, px, px}, ink);
            }
        }
    }
}

static void draw_image_at(flux_canvas *canvas, flux_image *image, float cx, float cy, float size,
                          float rotation, float sx, float sy, const flux_paint *paint) {
    flux_canvas_save(canvas);
    flux_canvas_translate(canvas, cx, cy);
    flux_canvas_rotate(canvas, rotation);
    flux_canvas_scale(canvas, sx, sy);
    flux_canvas_draw_image(canvas, image, (flux_rect){-size * 0.5f, -size * 0.5f, size, size},
                           paint);
    flux_canvas_restore(canvas);
}

static void draw_panel(flux_canvas *canvas, flux_rect panel) {
    flux_canvas_fill_rrect(canvas, panel, 22.0f, flux_color_rgba_premul(27, 31, 46, 245));
    flux_canvas_stroke_rrect(canvas, panel, 22.0f, flux_color_rgba_premul(80, 92, 125, 160), 1.5f);
}

static void on_resize(GLFWwindow *window, int width, int height) {
    flux_surface *surface = glfwGetWindowUserPointer(window);
    if (surface && width > 0 && height > 0)
        (void)flux_surface_resize(surface, (uint32_t)width, (uint32_t)height);
}

int main(void) {
    if (!glfwInit() || !glfwVulkanSupported()) {
        fprintf(stderr, "GLFW Vulkan initialization failed\n");
        return 1;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    GLFWwindow *window = glfwCreateWindow(1100, 720, "flux image animation", NULL, NULL);
    if (!window) {
        glfwTerminate();
        return 1;
    }

    uint32_t extension_count = 0;
    const char **instance_extensions = glfwGetRequiredInstanceExtensions(&extension_count);
    const char *device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    flux_pipeline_cache_file cache = FLUX_PIPELINE_CACHE_FILE_INIT;
    flux_pipeline_cache_file_set_default_path(&cache, "image_animation.bin");
    flux_device_desc device_desc = {
        .type = FLUX_TYPE_DEVICE_DESC,
        .log = flux_console_logger,
        .validation = FLUX_VALIDATION_AUTO,
        .required_instance_extensions = instance_extensions,
        .required_instance_extension_count = extension_count,
        .required_device_extensions = device_extensions,
        .required_device_extension_count = 1,
        .frames_in_flight = 2,
        .pipeline_cache_load = flux_pipeline_cache_file_load,
        .pipeline_cache_save = flux_pipeline_cache_file_save,
        .pipeline_cache_userdata = &cache,
    };
    flux_device *device = NULL;
    flux_result result = flux_device_create(&device_desc, &device);
    if (result != FLUX_OK)
        goto fail_window;

    VkSurfaceKHR vk_surface = VK_NULL_HANDLE;
    if (glfwCreateWindowSurface(flux_device_vk_instance(device), window, NULL, &vk_surface) !=
        VK_SUCCESS) {
        result = FLUX_ERROR_BACKEND_FAILURE;
        goto fail_device;
    }

    int framebuffer_width = 0, framebuffer_height = 0;
    glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
    flux_surface_desc surface_desc = {
        .type = FLUX_TYPE_SURFACE_DESC,
        .vk_surface_khr = vk_surface,
        .width = (uint32_t)framebuffer_width,
        .height = (uint32_t)framebuffer_height,
        .vsync = true,
    };
    flux_surface *surface = NULL;
    result = flux_surface_create(device, &surface_desc, &surface);
    if (result != FLUX_OK)
        goto fail_vk_surface;
    glfwSetWindowUserPointer(window, surface);
    glfwSetFramebufferSizeCallback(window, on_resize);

    flux_canvas_desc canvas_desc = FLUX_CANVAS_DESC_INIT;
    canvas_desc.surface = surface;
    flux_canvas *canvas = NULL;
    result = flux_canvas_create(&canvas_desc, &canvas);
    if (result != FLUX_OK)
        goto fail_surface;

    uint32_t card_a_pixels[CARD_SIZE * CARD_SIZE];
    uint32_t card_b_pixels[CARD_SIZE * CARD_SIZE];
    uint32_t sprite_pixels[SPRITE_FRAMES * SPRITE_SIZE * SPRITE_SIZE];
    make_card_pixels(card_a_pixels, 0);
    make_card_pixels(card_b_pixels, 1);
    make_sprite_pixels(sprite_pixels);
    flux_image *card_a = make_image(device, CARD_SIZE, CARD_SIZE, card_a_pixels);
    flux_image *card_b = make_image(device, CARD_SIZE, CARD_SIZE, card_b_pixels);
    flux_image *sprites =
        make_image(device, SPRITE_FRAMES * SPRITE_SIZE, SPRITE_SIZE, sprite_pixels);
    if (!card_a || !card_b || !sprites) {
        fprintf(stderr, "could not create animation textures\n");
        result = FLUX_ERROR_BACKEND_FAILURE;
        goto fail_images;
    }

    printf("image animation: bounce, squash/stretch, spin/pulse, cross-fade, sprite sheet\n");
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        flux_frame *frame = NULL;
        result = flux_surface_begin_frame(surface, NULL, &frame);
        if (result == FLUX_ERROR_SURFACE_LOST) {
            int width = 0, height = 0;
            glfwGetFramebufferSize(window, &width, &height);
            if (width > 0 && height > 0)
                (void)flux_surface_resize(surface, (uint32_t)width, (uint32_t)height);
            continue;
        }
        if (result == FLUX_ERROR_INVALID_STATE)
            continue;
        if (result != FLUX_OK)
            break;

        flux_color clear = flux_color_rgba(12, 14, 23, 255);
        result = flux_canvas_begin_frame(canvas, frame, &clear);
        if (result != FLUX_OK)
            break;

        flux_surface_info info;
        flux_surface_get_info(surface, &info);
        float width = (float)info.width;
        float height = (float)info.height;
        float time = (float)glfwGetTime();

        float margin = 24.0f;
        float gap = 18.0f;
        float panel_w = (width - margin * 2.0f - gap) * 0.5f;
        float panel_h = (height - margin * 2.0f - gap) * 0.5f;
        flux_rect panels[4] = {
            {margin, margin, panel_w, panel_h},
            {margin + panel_w + gap, margin, panel_w, panel_h},
            {margin, margin + panel_h + gap, panel_w, panel_h},
            {margin + panel_w + gap, margin + panel_h + gap, panel_w, panel_h},
        };
        for (int i = 0; i < 4; ++i)
            draw_panel(canvas, panels[i]);

        /* 1. Bounce + squash/stretch. Contact is concentrated around the
         * ground frames; the airborne card relaxes to its natural aspect. */
        {
            flux_rect p = panels[0];
            draw_label(canvas, p.x + 22.0f, p.y + 20.0f, "BOUNCE");
            float phase = fmodf(time, 1.35f) / 1.35f;
            float airborne = sinf(phase * PI);
            float contact = powf(1.0f - airborne, 5.0f);
            float sx = 1.0f + 0.22f * contact;
            float sy = 1.0f - 0.18f * contact;
            float size = fminf(126.0f, p.h * 0.40f);
            float ground = p.y + p.h - 38.0f - size * 0.5f;
            float hop = fmaxf(38.0f, p.h * 0.35f);
            draw_image_at(canvas, card_a, p.x + p.w * 0.5f, ground - hop * airborne, size, 0.0f, sx,
                          sy, NULL);
        }

        /* 2. Arbitrary image rotation now remains UV-correct because the UVs
         * travel with the quad vertices. */
        {
            flux_rect p = panels[1];
            draw_label(canvas, p.x + 22.0f, p.y + 20.0f, "SPIN PULSE");
            float pulse = 1.0f + 0.09f * sinf(time * 3.2f);
            float size = fminf(146.0f, p.h * 0.48f);
            draw_image_at(canvas, card_b, p.x + p.w * 0.5f, p.y + p.h * 0.57f, size, time * 0.85f,
                          pulse, pulse, NULL);
        }

        /* 3. A smooth A -> B -> A cross-fade. A stays opaque and B is
         * composited over it, which gives the expected linear mix under
         * premultiplied SRC_OVER. */
        {
            flux_rect p = panels[2];
            draw_label(canvas, p.x + 22.0f, p.y + 20.0f, "CROSS FADE");
            float mix = 0.5f - 0.5f * cosf(time * PI * 0.65f);
            mix = smoothstep01(mix);
            float size = fminf(154.0f, p.h * 0.50f);
            float cx = p.x + p.w * 0.5f, cy = p.y + p.h * 0.58f;
            draw_image_at(canvas, card_a, cx, cy, size, 0.0f, 1.0f, 1.0f, NULL);
            flux_paint fade = flux_paint_solid(
                flux_color_rgba_premul(255, 255, 255, (uint8_t)(mix * 255.0f + 0.5f)));
            draw_image_at(canvas, card_b, cx, cy, size, 0.0f, 1.0f, 1.0f, &fade);
        }

        /* 4. Sprite-sheet frame selection plus eased horizontal motion. */
        {
            flux_rect p = panels[3];
            draw_label(canvas, p.x + 22.0f, p.y + 20.0f, "SPRITE");
            int frame_index = (int)floorf(time * 10.0f) % SPRITE_FRAMES;
            float travel = fmodf(time * 0.24f, 1.0f);
            float ping_pong = travel < 0.5f ? travel * 2.0f : (1.0f - travel) * 2.0f;
            ping_pong = smoothstep01(ping_pong);
            float size = fminf(128.0f, p.h * 0.43f);
            float left = p.x + 30.0f + size * 0.5f;
            float right = p.x + p.w - 30.0f - size * 0.5f;
            float x = left + (right - left) * ping_pong;
            float y = p.y + p.h * 0.60f + sinf(time * 8.0f) * 8.0f;
            flux_rect src = {(float)frame_index / (float)SPRITE_FRAMES, 0.0f,
                             1.0f / (float)SPRITE_FRAMES, 1.0f};
            flux_canvas_draw_image_sub(
                canvas, sprites, (flux_rect){x - size * 0.5f, y - size * 0.5f, size, size}, src);
        }

        flux_canvas_end_frame(canvas);
        result = flux_frame_submit(frame);
        if (result != FLUX_OK)
            break;
        result = flux_frame_present(frame);
        if (result != FLUX_OK && result != FLUX_ERROR_SURFACE_LOST)
            break;
    }

fail_images:
    flux_device_wait_idle(device);
    if (sprites)
        flux_image_release(sprites);
    if (card_b)
        flux_image_release(card_b);
    if (card_a)
        flux_image_release(card_a);
    flux_canvas_destroy(canvas);
fail_surface:
    flux_surface_release(surface);
fail_vk_surface:
    vkDestroySurfaceKHR(flux_device_vk_instance(device), vk_surface, NULL);
fail_device:
    flux_device_release(device);
fail_window:
    glfwDestroyWindow(window);
    glfwTerminate();
    return result == FLUX_OK || result == FLUX_ERROR_SURFACE_LOST ? 0 : (int)result;
}
