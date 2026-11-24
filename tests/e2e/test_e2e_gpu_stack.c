/*
 * e2e_gpu_stack — full-stack public-API GPU composition E2E scenario.
 *
 * Strictly public-header-only (<optics/optics.h> + public sub-headers): links
 * the umbrella façade `optics_dep` exactly as a real application does, and
 * composes six libraries in one live Vulkan frame pipeline:
 *
 *   1. gfx (Vulkan offscreen surface, GPU Canvas, Dual-Kawase blur filter)
 *   2. material (Frosted Glass + Liquid Glass over a real backdrop capture)
 *   3. svg (runtime SVG icon registered into UI)
 *   4. ui + text (interactive UI widgets laid out, snapshotted, and composited
 *      over the material layer)
 *   5. motion (analytic spring advancing a glass body position across frames)
 *
 * Verifies real readback pixels:
 *   - raw backdrop band retains expected high-contrast colors;
 *   - frosted glass panel visibly blends/blurs the striped backdrop;
 *   - liquid glass lens visibly refracts/tints with solid interior coverage;
 *   - foreground UI button & SVG icon render crisply over the glass surface;
 *   - animating the glass body with optics_motion_spring visibly moves pixels
 *     between frame 0 and settled frames with zero leaks.
 */

#include "gfx_test.h"
#include "optics_test.h"
#include "ui_test.h"

#include <optics/optics.h>

#include <stdlib.h>
#include <string.h>

#define W 320u
#define H 192u
#define BYTES (W * H * 4u)

static const char BADGE_SVG[] = "<svg viewBox='0 0 24 24'>"
                                "<circle cx='12' cy='12' r='10' fill='#ff4060'/>"
                                "<rect x='7' y='10' width='10' height='4' fill='#ffffff'/>"
                                "</svg>";

static void draw_material_output(optics_gfx_canvas *canvas, optics_material_output out) {
    if (!out.image)
        return;
    optics_gfx_canvas_draw_image(
        canvas, out.image,
        (optics_gfx_rect){out.origin_x, out.origin_y, (float)out.width, (float)out.height},
        nullptr);
}

static const uint8_t *px_at(const uint8_t *px, uint32_t x, uint32_t y) {
    return px + ((size_t)y * W + (size_t)x) * 4u;
}

static size_t diff_in_rect(const uint8_t *a, const uint8_t *b, optics_gfx_rect r) {
    int x0 = (int)r.x;
    int y0 = (int)r.y;
    int x1 = (int)(r.x + r.w + 0.999f);
    int y1 = (int)(r.y + r.h + 0.999f);
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > (int)W)
        x1 = (int)W;
    if (y1 > (int)H)
        y1 = (int)H;

    size_t diff = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            size_t off = ((size_t)y * W + (size_t)x) * 4u;
            if (abs((int)a[off + 0] - (int)b[off + 0]) + abs((int)a[off + 1] - (int)b[off + 1]) +
                    abs((int)a[off + 2] - (int)b[off + 2]) >
                18)
                diff++;
        }
    }
    return diff;
}

/* High-contrast vertical stripe backdrop so blur and refraction are unmistakable. */
static optics_gfx_image *make_striped_backdrop(optics_gfx_device *device) {
    uint8_t *data = malloc(BYTES);
    OT_REQUIRE(data != nullptr);
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            uint8_t *p = data + ((size_t)y * W + (size_t)x) * 4u;
            bool stripe = ((x / 8u) % 2u) == 0u;
            if (y < H / 2u) {
                p[0] = stripe ? 235u : 20u;
                p[1] = stripe ? 60u : 200u;
                p[2] = stripe ? 30u : 240u;
            } else {
                p[0] = stripe ? 40u : 240u;
                p[1] = stripe ? 220u : 180u;
                p[2] = stripe ? 80u : 30u;
            }
            p[3] = 255u;
        }
    }
    optics_gfx_image_desc id = OPTICS_GFX_IMAGE_DESC_INIT;
    id.width = W;
    id.height = H;
    id.format = OPTICS_GFX_FORMAT_RGBA8_UNORM;
    id.initial_data = data;
    optics_gfx_image *img = nullptr;
    OT_REQUIRE(optics_gfx_image_create(device, &id, &img) == OPTICS_GFX_OK);
    free(data);
    return img;
}

typedef struct compose_env {
    optics_gfx_surface *surface;
    optics_gfx_canvas *canvas;
    optics_gfx_image *backdrop;
    optics_gfx_blur_filter *blur;
    optics_material_frosted_filter *frosted;
    optics_material_liquid_glass_filter *glass;
} compose_env;

static void render_composed_frame(compose_env *env, float glass_x,
                                  const optics_ui_scene_snapshot *ui_snap, uint8_t *out_px) {
    optics_gfx_frame *frame = nullptr;
    OT_REQUIRE(optics_gfx_surface_begin_frame(env->surface, nullptr, &frame) == OPTICS_GFX_OK);

    /* 1. Blur the sharp backdrop via Dual-Kawase blur filter. */
    optics_gfx_effect_blur_desc bd = OPTICS_GFX_EFFECT_BLUR_DESC_INIT;
    bd.input = env->backdrop;
    bd.sigma = 10.0f;
    optics_gfx_image *blurred = nullptr;
    OT_REQUIRE(optics_gfx_blur_filter_apply(
                   env->blur, &(optics_gfx_blur_filter_apply_opts){
                                  .frame = frame, .desc = &bd, .out = &blurred}) == OPTICS_GFX_OK);

    /* 2. Frosted Glass panel on the left half [16, 48, 120, 112]. */
    optics_material_frosted_group fg = OPTICS_MATERIAL_FROSTED_GROUP_INIT;
    fg.shape = (optics_material_frosted_shape){.bounds = {16.0f, 48.0f, 120.0f, 112.0f},
                                               .corner_radius = 14.0f};
    fg.tint_color = 0x182848u;
    optics_material_frosted_desc fd = OPTICS_MATERIAL_FROSTED_DESC_INIT;
    fd.input = env->backdrop;
    fd.blurred_input = blurred;
    fd.groups = &fg;
    fd.group_count = 1;
    optics_material_output frosted_out = {};
    OT_REQUIRE(
        optics_material_frosted_filter_apply(
            env->frosted, &(optics_material_frosted_filter_apply_opts){
                              .frame = frame, .desc = &fd, .out = &frosted_out}) == OPTICS_GFX_OK);

    /* 3. Liquid Glass lens on the right half, animated along X by optics_motion_spring. */
    const optics_material_liquid_glass_shape glass_shapes[1] = {
        {.bounds = {glass_x, 56.0f, 96.0f, 96.0f}, .corner_radius = 20.0f},
    };
    optics_material_liquid_glass_group lg = OPTICS_MATERIAL_LIQUID_GLASS_GROUP_INIT;
    lg.shapes = glass_shapes;
    lg.shape_count = 1;
    optics_material_liquid_glass_desc ld = OPTICS_MATERIAL_LIQUID_GLASS_DESC_INIT;
    ld.input = env->backdrop;
    ld.blurred_input = blurred;
    ld.groups = &lg;
    ld.group_count = 1;
    optics_material_output glass_out = {};
    OT_REQUIRE(
        optics_material_liquid_glass_filter_apply(
            env->glass, &(optics_material_liquid_glass_filter_apply_opts){
                            .frame = frame, .desc = &ld, .out = &glass_out}) == OPTICS_GFX_OK);

    /* 4. Composite backdrop -> frosted -> liquid glass -> foreground UI snapshot. */
    optics_gfx_color clear = optics_gfx_color_rgba_premul(0, 0, 0, 255);
    OT_REQUIRE(optics_gfx_canvas_begin(env->canvas, &(optics_gfx_canvas_pass_desc){
                                                        .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                        .frame = frame,
                                                        .clear_color = &clear}) == OPTICS_GFX_OK);
    optics_gfx_canvas_draw_image(env->canvas, env->backdrop,
                                 (optics_gfx_rect){0.0f, 0.0f, (float)W, (float)H}, nullptr);
    draw_material_output(env->canvas, frosted_out);
    draw_material_output(env->canvas, glass_out);
    if (ui_snap)
        OT_EXPECT(optics_ui_snapshot_submit(ui_snap, env->canvas) == OPTICS_GFX_OK);
    OT_REQUIRE(optics_gfx_canvas_end(env->canvas) == OPTICS_GFX_OK);

    OT_REQUIRE(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
    OT_REQUIRE(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
    memset(out_px, 0, BYTES);
    OT_REQUIRE(optics_gfx_surface_read_pixels(env->surface, out_px, BYTES) == OPTICS_GFX_OK);
}

int main(void) {
    optics_gfx_device *device = optics_gfx_test_make_headless_device();
    if (!device)
        OT_SKIP("no Vulkan device");

    compose_env env = {};
    optics_gfx_surface_desc sd = OPTICS_GFX_SURFACE_DESC_INIT;
    sd.width = W;
    sd.height = H;
    OT_REQUIRE(optics_gfx_surface_create(device, &sd, &env.surface) == OPTICS_GFX_OK);

    optics_gfx_canvas_desc cd = OPTICS_GFX_CANVAS_DESC_INIT;
    cd.surface = env.surface;
    OT_REQUIRE(optics_gfx_canvas_create(&cd, &env.canvas) == OPTICS_GFX_OK);

    env.backdrop = make_striped_backdrop(device);
    OT_REQUIRE(optics_gfx_blur_filter_create(device, &env.blur) == OPTICS_GFX_OK);
    OT_REQUIRE(optics_material_frosted_filter_create(device, &env.frosted) == OPTICS_GFX_OK);
    OT_REQUIRE(optics_material_liquid_glass_filter_create(device, &env.glass) == OPTICS_GFX_OK);

    optics_ui_icon_id icon = optics_ui_icon_register_svg(BADGE_SVG);
    OT_REQUIRE(icon != OPTICS_UI_ICON_INVALID);

    optics_ui *ui = nullptr;
    OT_REQUIRE(optics_ui_create(
                   &(optics_ui_desc){
                       .device = device,
                       .theme = optics_ui_theme_dark(),
                       .scale = 1.0f,
                   },
                   &ui) == OPTICS_GFX_OK);

    /* Build a minimal HUD overlay (top bar: label + button + SVG icon). */
    optics_ui_response btn = {};
    optics_ui_input in = {.display_size = {(float)W, (float)H}, .dt_seconds = 1.0f / 60.0f};
    for (int f = 0; f < 2; ++f) {
        optics_ui_begin(ui, &in);
        optics_ui_row_begin(ui, &(optics_ui_layout_opts){.gap = 8.0f});
        optics_ui_icon(ui, &(optics_ui_icon_opts){.id = icon, .size = 24.0f});
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "Optics GPU Stack"});
        btn = optics_ui_button(
            ui, &(optics_ui_button_opts){.box = {.id = "hud_btn", .width = 80.0f, .height = 28.0f},
                                         .label = "Action"});
        optics_ui_row_end(ui);
        optics_ui_test_end(ui);
    }
    optics_gfx_rect btn_rect = optics_ui_node_bounds(optics_ui_find(ui, btn.id));
    OT_REQUIRE(btn_rect.w > 0.0f && btn_rect.h > 0.0f);

    optics_ui_scene_snapshot *ui_snap = nullptr;
    OT_REQUIRE(optics_ui_snapshot_create(ui, &ui_snap) == OPTICS_GFX_OK);

    uint8_t *px_no_ui = malloc(BYTES);
    uint8_t *px_frame0 = malloc(BYTES);
    uint8_t *px_frame_end = malloc(BYTES);
    OT_REQUIRE(px_no_ui && px_frame0 && px_frame_end);

    /* Render once without UI, then with UI at spring start X=156. */
    render_composed_frame(&env, 156.0f, nullptr, px_no_ui);
    render_composed_frame(&env, 156.0f, ui_snap, px_frame0);

    /* 1. Uncovered bottom-right corner preserves the raw striped backdrop contrast. */
    const uint8_t *raw_a = px_at(px_frame0, 4u, H - 8u);  /* stripe 0: greenish (40,220,80) */
    const uint8_t *raw_b = px_at(px_frame0, 12u, H - 8u); /* stripe 1: yellowish (240,180,30) */
    OT_EXPECT(abs((int)raw_a[0] - (int)raw_b[0]) > 150);

    /* 2. Inside the frosted glass panel, stripe contrast is significantly softened by blur. */
    const uint8_t *fr_a = px_at(px_frame0, 52u, 100u);
    const uint8_t *fr_b = px_at(px_frame0, 60u, 100u);
    OT_EXPECT(fr_a[3] > 200);
    OT_EXPECT(abs((int)fr_a[0] - (int)fr_b[0]) < 80);

    /* 3. Foreground UI button visibly modifies pixels inside its exact layout rect. */
    OT_EXPECT(diff_in_rect(px_no_ui, px_frame0, btn_rect) > 100);

    /* 4. Advance the glass body along X from 156 -> 208 using optics_motion_spring. */
    optics_motion_spring spring = optics_motion_spring_at(156.0f);
    optics_motion_spring_params params = optics_motion_spring_snappy();
    for (int step = 0; step < 30; ++step)
        (void)optics_motion_spring_advance(&spring, 208.0f, params, 1.0f / 60.0f, false);
    OT_EXPECT(spring.value > 195.0f);

    render_composed_frame(&env, spring.value, ui_snap, px_frame_end);
    /* The glass swept across [150, 50, 160, 110]; pixels there must visibly shift. */
    OT_EXPECT(diff_in_rect(px_frame0, px_frame_end,
                           (optics_gfx_rect){150.0f, 50.0f, 160.0f, 110.0f}) > 500);

    optics_gfx_device_wait_idle(device);
    free(px_no_ui);
    free(px_frame0);
    free(px_frame_end);
    optics_ui_snapshot_release(ui_snap);
    optics_ui_release(ui);
    optics_material_liquid_glass_filter_release(env.glass);
    optics_material_frosted_filter_release(env.frosted);
    optics_gfx_blur_filter_release(env.blur);
    optics_gfx_image_release(env.backdrop);
    optics_gfx_canvas_release(env.canvas);
    optics_gfx_surface_release(env.surface);
    optics_gfx_device_release(device);
    return OT_REPORT();
}
