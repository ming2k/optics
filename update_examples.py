import re

def update_file(filepath):
    with open(filepath, 'r') as f:
        content = f.read()

    # 1. Update draw_chaos
    new_draw_chaos = """static void draw_chaos(flux_canvas *c, flux_arena *arena, float W, float H, flux_image *noise_img) {
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
        flux_paint g = flux_paint_radial_gradient((flux_point){W * 0.85f, H * 0.15f}, W * 0.6f, stops, 2);
        flux_path *p = nullptr; flux_path_create(&p, arena);
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
        flux_paint g = flux_paint_radial_gradient((flux_point){W * 0.15f, H * 0.85f}, W * 0.6f, stops, 2);
        flux_path *p = nullptr; flux_path_create(&p, arena);
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
}"""

    content = re.sub(r'static void draw_chaos\(.*?\)\s*\{.*?\n\}', new_draw_chaos, content, flags=re.DOTALL)

    # 2. Add noise image creation logic after target creation
    noise_logic = """
    /* Create static noise texture for grain. */
    flux_image *noise_img = nullptr;
    {
        uint8_t *noise_data = (uint8_t*)malloc(256 * 256 * 4);
        if (noise_data) {
            for (int i = 0; i < 256 * 256 * 4; i += 4) {
                uint8_t v = rand() % 256;
                uint8_t a = 10 + (rand() % 15);
                noise_data[i] = (uint8_t)((v * a) / 255);
                noise_data[i+1] = (uint8_t)((v * a) / 255);
                noise_data[i+2] = (uint8_t)((v * a) / 255);
                noise_data[i+3] = a;
            }
            flux_image_desc ndesc = FLUX_IMAGE_DESC_INIT;
            ndesc.width = 256; ndesc.height = 256;
            ndesc.format = FLUX_FORMAT_RGBA8_UNORM;
            ndesc.initial_data = noise_data;
            flux_image_create(device, &ndesc, &noise_img);
            free(noise_data);
        }
    }
    """
    if "flux_image *capture" in content:
        # insert after target creation check
        content = re.sub(r'(if \(flux_image_create_render_target[^{]*\{.*?return 1;\n\s*\})', r'\1' + noise_logic, content, flags=re.DOTALL)

    # Update draw_chaos calls to pass noise_img instead of t
    content = content.replace("draw_chaos(canvas, &arena, W, H, t);", "draw_chaos(canvas, &arena, W, H, noise_img);")

    # 3. Increase blur sigma for the glass (to completely blur out the noise)
    content = content.replace("bd.sigma = 6.0f;", "bd.sigma = 16.0f;")

    # 4. Replace the compositing logic
    new_composite = """
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
            flux_paint fres = flux_paint_radial_gradient((flux_point){cx, cy}, gw * 0.55f, stops, 4);
            flux_canvas_fill_path(canvas, shape, &fres);
            
            flux_gradient_stop s_stops[3] = {
                {0.00f, flux_color_rgba_premul(255, 255, 255, 90)},
                {0.45f, flux_color_rgba_premul(255, 255, 255, 20)},
                {1.00f, flux_color_rgba_premul(0, 0, 0, 0)},
            };
            flux_paint sheen = flux_paint_linear_gradient((flux_point){gx, gy}, (flux_point){gx, gy + gh * 0.45f}, s_stops, 3);
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
        flux_path *p_sun = nullptr; flux_path_create(&p_sun, &arena);
        if (p_sun) {
            flux_path_add_circle(p_sun, ix_sun, iy, 7.0f);
            for (int i=0; i<8; i++) {
                float a = i * 3.14159f / 4.0f;
                flux_path_move_to(p_sun, ix_sun + cosf(a)*11.0f, iy + sinf(a)*11.0f);
                flux_path_line_to(p_sun, ix_sun + cosf(a)*15.0f, iy + sinf(a)*15.0f);
            }
            flux_canvas_stroke_path(canvas, p_sun, &sp_icon);
        }

        /* Moon icon */
        flux_path *p_moon = nullptr; flux_path_create(&p_moon, &arena);
        if (p_moon) {
            float mr = 13.0f;
            flux_path_move_to(p_moon, ix_moon + mr*0.3f, iy - mr);
            flux_path_cubic_to(p_moon, ix_moon - mr*1.2f, iy - mr, ix_moon - mr*1.2f, iy + mr, ix_moon + mr*0.3f, iy + mr);
            flux_path_cubic_to(p_moon, ix_moon - mr*0.2f, iy + mr*0.5f, ix_moon - mr*0.2f, iy - mr*0.5f, ix_moon + mr*0.3f, iy - mr);
            flux_canvas_stroke_path(canvas, p_moon, &sp_icon);
        }

        /* Sunrise icon */
        flux_path *p_sunrise = nullptr; flux_path_create(&p_sunrise, &arena);
        if (p_sunrise) {
            float r = 10.0f;
            float k = r * 0.55228f;
            float ry = iy + 3.0f;
            flux_path_move_to(p_sunrise, ix_sunrise - r, ry);
            flux_path_cubic_to(p_sunrise, ix_sunrise - r, ry - k, ix_sunrise - k, ry - r, ix_sunrise, ry - r);
            flux_path_cubic_to(p_sunrise, ix_sunrise + k, ry - r, ix_sunrise + r, ry - k, ix_sunrise + r, ry);
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
"""
    # Replace the composite section
    # The composite section starts at "/* Sharp backdrop: draw the captured scene. */"
    # And ends at "flux_arena_reset(&arena);"
    content = re.sub(r'/\* Sharp backdrop: draw the captured scene.*?flux_arena_reset\(&arena\);', new_composite + "\n        flux_arena_reset(&arena);", content, flags=re.DOTALL)

    # 5. Clean up noise_img
    cleanup = "if (noise_img) flux_image_release(noise_img);\n    if (capture)"
    content = content.replace("if (capture)", cleanup)

    with open(filepath, 'w') as f:
        f.write(content)

update_file('examples/flux/liquid_glass.c')
update_file('examples/flux/liquid_glass_shot.c')
