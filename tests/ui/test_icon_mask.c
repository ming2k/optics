/*
 * Icon coverage-mask cache contracts (ADR-0112).
 *
 * Two things must hold, and neither is observable from pixel snapshots alone:
 *
 *   1. A mask is baked at DEVICE resolution and blitted 1:1. If the bake size
 *      and the key ever disagreed, or if the factor stopped propagating, icons
 *      would silently fall back to the low-coverage vector path the cache exists
 *      to replace.
 *   2. An icon's rendered ink is CENTRED in its box. This is the regression that
 *      a fractional scale used to break: the origin was rounded in logical space
 *      after already being snapped in device space, so at scale 1.25/1.5 the
 *      glyph landed between device pixels. A centroid check catches a
 *      systematic offset that "looks fine" in a rendered thumbnail.
 *
 * The mask accessors are internal, so this binary compiles icon_mask.c (and the
 * icon registry it resolves through) directly — the same pattern test_drawlist_hash
 * and test_icon_runtime_content use.
 */
#include "../../../libs/ui/src/internal.h"
#include "../../../libs/ui/src/render/icon_mask.h"
#include "optics_test.h"
#include "ui_test.h"

/* This test compiles ui's icon_mask.c directly, and that TU now allocates
 * through the ui funnel (ADR-0135), which lives in context.c and is hidden in
 * the shared library. Provide the same funnel here so the direct-compiled TU
 * links: same owner/kind attribution, same device-less fallback. */
void *optics_ui__alloc(optics_ui *ui, size_t bytes);
void optics_ui__free(optics_ui *ui, void *ptr);
#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
void *optics_ui__alloc(optics_ui *ui, size_t bytes) {
    if (!ui->device)
        return calloc(1, bytes);
    void *p = nullptr;
    if (optics_gfx_device_alloc(ui->device,
                                &(optics_gfx_device_alloc_opts){
                                    .type = OPTICS_GFX_TYPE_DEVICE_ALLOC_OPTS,
                                    .bytes = bytes,
                                    .owner = "ui/core",
                                    .kind = "host",
                                    .scope = OPTICS_GFX_SCOPE_CONTEXT,
                                },
                                &p) != OPTICS_GFX_OK)
        return nullptr;
    return p;
}
#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
void optics_ui__free(optics_ui *ui, void *ptr) {
    if (!ptr)
        return;
    if (!ui->device) {
        free(ptr);
        return;
    }
    optics_gfx_device_free(ui->device, &(optics_gfx_device_free_opts){
                                           .type = OPTICS_GFX_TYPE_DEVICE_FREE_OPTS,
                                           .ptr = ptr,
                                           .owner = "ui/core",
                                           .kind = "host",
                                           .scope = OPTICS_GFX_SCOPE_CONTEXT,
                                       });
}
#include <math.h>
#include <optics/gfx/canvas.h>
#include <optics/gfx/canvas_cpu.h>
#include <optics/ui/icon.h>

/* Count distinct partial-coverage values in a rendered icon's alpha channel. */
static int icon_partial_levels(optics_ui_icon_id id, float size) {
    optics_ui *ui = nullptr;
    if (optics_ui_create(&(optics_ui_desc){.scale = 1.0f}, &ui) != OPTICS_GFX_OK)
        return -1;
    const uint32_t w = 32, h = 32;
    optics_ui_input in = {.display_size = {(float)w, (float)h}, .dt_seconds = 0.016f};
    optics_ui_begin(ui, &in);
    optics_ui_icon(ui, &(optics_ui_icon_opts){.id = id, .size = size});
    optics_ui_end(ui);

    optics_gfx_canvas *c = nullptr;
    if (optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = w,
                                                           .height = h},
                                 &c) != OPTICS_GFX_OK) {
        optics_ui_release(ui);
        return -1;
    }
    optics_gfx_color clear = {};
    if (optics_gfx_canvas_begin(
            c, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                              .clear_color = &clear}) == OPTICS_GFX_OK) {
        optics_ui_scene_snapshot *s = nullptr;
        if (optics_ui_snapshot_create(ui, &s) == OPTICS_GFX_OK) {
            (void)optics_ui_snapshot_submit(s, c);
            optics_ui_snapshot_release(s);
        }
        (void)optics_gfx_canvas_end(c);
    }
    uint32_t pw = 0, ph = 0, st = 0;
    const uint8_t *px = optics_gfx_canvas_cpu_pixels(c, &pw, &ph, &st);
    int seen[256] = {};
    if (px) {
        for (uint32_t y = 0; y < ph; ++y)
            for (uint32_t x = 0; x < pw; ++x)
                seen[px[(size_t)y * st + (size_t)x * 4 + 3]] = 1;
    }
    int levels = 0;
    for (int i = 1; i < 255; ++i)
        levels += seen[i];
    optics_gfx_canvas_release(c);
    optics_ui_release(ui);
    return levels;
}

/* Coverage-weighted centroid of an icon rendered into a `box` box at `scale`. */
static bool icon_centroid(optics_ui_icon_id id, float size, float scale, uint32_t box,
                          double *out_cx, double *out_cy) {
    optics_ui *ui = nullptr;
    if (optics_ui_create(&(optics_ui_desc){.scale = scale}, &ui) != OPTICS_GFX_OK)
        return false;
    uint32_t w = (uint32_t)(box * scale), h = w;
    optics_ui_input in = {.display_size = {(float)box, (float)box}, .dt_seconds = 0.016f};
    optics_ui_begin(ui, &in);
    optics_ui_icon(ui, &(optics_ui_icon_opts){.id = id, .size = size});
    optics_ui_end(ui);

    /* Content scale stays 1.0: ui already applies ui->scale internally, so a
     * scaled canvas here would apply it twice. */
    optics_gfx_canvas *c = nullptr;
    if (optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = w,
                                                           .height = h},
                                 &c) != OPTICS_GFX_OK) {
        optics_ui_release(ui);
        return false;
    }
    optics_gfx_color clear = {};
    if (optics_gfx_canvas_begin(
            c, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                              .clear_color = &clear}) == OPTICS_GFX_OK) {
        optics_ui_scene_snapshot *s = nullptr;
        if (optics_ui_snapshot_create(ui, &s) == OPTICS_GFX_OK) {
            (void)optics_ui_snapshot_submit(s, c);
            optics_ui_snapshot_release(s);
        }
        (void)optics_gfx_canvas_end(c);
    }
    uint32_t pw = 0, ph = 0, st = 0;
    const uint8_t *px = optics_gfx_canvas_cpu_pixels(c, &pw, &ph, &st);
    bool ok = false;
    if (px) {
        double sx = 0, sy = 0, sw = 0;
        for (uint32_t y = 0; y < ph; ++y) {
            for (uint32_t x = 0; x < pw; ++x) {
                unsigned a = px[(size_t)y * st + (size_t)x * 4 + 3];
                if (!a)
                    continue;
                sx += (double)x * a;
                sy += (double)y * a;
                sw += a;
            }
        }
        if (sw > 0) {
            *out_cx = sx / sw;
            *out_cy = sy / sw;
            ok = true;
        }
    }
    optics_gfx_canvas_release(c);
    optics_ui_release(ui);
    return ok;
}

/* alert_circle's ink is symmetric about the 24-unit box centre, so its centroid
 * is the box centre regardless of stroke details. */
static void check_centred_at_scale(float scale) {
    const float size = 14.0f;
    uint32_t box = 24;
    double cx = 0, cy = 0;
    OT_EXPECT(icon_centroid(OPTICS_UI_ICON_ALERT_CIRCLE, size, scale, box, &cx, &cy));
    /* The icon node fills `size` logical px at the layout origin; its centre is
     * size*scale/2 in device px, minus half a pixel for pixel-centre indexing. */
    double expect = (double)size * scale * 0.5 - 0.5;
    OT_EXPECT(fabs(cx - expect) < 0.6);
    OT_EXPECT(fabs(cy - expect) < 0.6);
}

int main(void) {
    /* ---- mask cache: baked at device size, keyed by device stroke ---- */
    {
        optics_ui *ui = nullptr;
        OT_EXPECT(optics_ui_create(&(optics_ui_desc){.scale = 1.0f}, &ui) == OPTICS_GFX_OK);

        float stroke = 2.0f * 20.0f / 24.0f;
        optics_ui__icon_mask *a = optics_ui__icon_mask_acquire(
            ui, OPTICS_UI_ICON_CHECK, 20u, stroke, 0.0f, 0u, OPTICS_UI_ICON_MASK_BODY);
        OT_EXPECT(a != nullptr);
        if (a) {
            OT_EXPECT(a->dev_px == 20u);
            OT_EXPECT(a->w == 20u + 2u * a->halo);
            OT_EXPECT(a->h == a->w);
            /* Device-less ui: the mask lives as host bytes, not an image. */
            OT_EXPECT(a->host != nullptr);
            OT_EXPECT(a->image == nullptr);
            int nonzero = 0;
            for (uint32_t i = 0; i < a->w * a->h; ++i)
                if (a->host[i])
                    nonzero++;
            OT_EXPECT(nonzero > 0);
            /* Full-coverage ink must exist: a stroke thinner than one pixel
             * would top out below 255 and read as a permanently grey glyph. */
            int peak = 0;
            for (uint32_t i = 0; i < a->w * a->h; ++i)
                if (a->host[i] > peak)
                    peak = a->host[i];
            OT_EXPECT(peak == 255);
        }

        /* Same request returns the same entry — no duplicate bakes. */
        optics_ui__icon_mask *again = optics_ui__icon_mask_acquire(
            ui, OPTICS_UI_ICON_CHECK, 20u, stroke, 0.0f, 0u, OPTICS_UI_ICON_MASK_BODY);
        OT_EXPECT(again == a);

        /* A different device size is a different entry. */
        optics_ui__icon_mask *bigger = optics_ui__icon_mask_acquire(
            ui, OPTICS_UI_ICON_CHECK, 24u, stroke, 0.0f, 0u, OPTICS_UI_ICON_MASK_BODY);
        OT_EXPECT(bigger != nullptr && bigger != a);
        if (bigger)
            OT_EXPECT(bigger->dev_px == 24u);

        /* An outline layer is its own entry, and an outline layer with no
         * outline width is meaningless rather than silently a body mask. */
        optics_ui__icon_mask *outline = optics_ui__icon_mask_acquire(
            ui, OPTICS_UI_ICON_CHECK, 20u, stroke, 1.5f, 0u, OPTICS_UI_ICON_MASK_OUTLINE);
        OT_EXPECT(outline != nullptr && outline != a);
        OT_EXPECT(optics_ui__icon_mask_acquire(ui, OPTICS_UI_ICON_CHECK, 20u, stroke, 0.0f, 0u,
                                               OPTICS_UI_ICON_MASK_OUTLINE) == nullptr);

        /* A body-layer mask never carries the outline, so asking with an outline
         * width must not mint a second entry with byte-identical coverage. */
        optics_ui__icon_mask *body_dup = optics_ui__icon_mask_acquire(
            ui, OPTICS_UI_ICON_CHECK, 20u, stroke, 1.5f, 0u, OPTICS_UI_ICON_MASK_BODY);
        OT_EXPECT(body_dup == a);

        /* An unknown run index and a zero device size are rejected, so a caller
         * falls back instead of masking nothing. */
        OT_EXPECT(optics_ui__icon_mask_acquire(ui, OPTICS_UI_ICON_CHECK, 20u, stroke, 0.0f, 7u,
                                               OPTICS_UI_ICON_MASK_BODY) == nullptr);
        OT_EXPECT(optics_ui__icon_mask_acquire(ui, OPTICS_UI_ICON_CHECK, 0u, stroke, 0.0f, 0u,
                                               OPTICS_UI_ICON_MASK_BODY) == nullptr);

        optics_ui_release(ui); /* must free every cached mask without leaking */
    }

    /* ---- rendered icons carry far more coverage than 4x MSAA allows ---- */
    {
        /* 4x MSAA can express at most 3 partial levels. A 14 px stroked icon
         * through the mask path must clear that comfortably. */
        int chevron = icon_partial_levels(OPTICS_UI_ICON_CHEVRON_RIGHT, 14.0f);
        int circle = icon_partial_levels(OPTICS_UI_ICON_ALERT_CIRCLE, 14.0f);
        OT_EXPECT(chevron > 3);
        OT_EXPECT(circle > 3);
        int star = icon_partial_levels(OPTICS_UI_ICON_STAR_ROUNDED_FILLED, 20.0f);
        OT_EXPECT(star > 3);
    }

    /* ---- placement is device-grid exact at every scale ---- */
    check_centred_at_scale(1.0f);
    check_centred_at_scale(1.25f);
    check_centred_at_scale(1.5f);
    check_centred_at_scale(2.0f);

    /* ---- filled (non-stroke) icons bake and render ---- */
    {
        int filled = icon_partial_levels(OPTICS_UI_ICON_STAR_ROUNDED_FILLED, 16.0f);
        int outline = icon_partial_levels(OPTICS_UI_ICON_STAR_ROUNDED, 16.0f);
        OT_EXPECT(filled > 0);
        OT_EXPECT(outline > 0);
    }

    /* ---- run resolution is the single source of truth ---- *
     * The vector fallback's old bug was resolving an icon's paint runs
     * differently from the mask path (collapsing the stream to one paint), which
     * silently discarded every per-shape colour. These two functions are now the
     * only run resolver, so pin their contract directly: a hand-built descriptor
     * with three runs must resolve to exactly three spans with their colours and
     * fill/stroke modes intact, and each colour must survive resolution.
     *
     * End-to-end rendering of a multi-colour runtime SVG is covered separately by
     * test_icon_mask_render, which links only the shared library (see
     * meson.build for why that must not compile a second icon registry). */
    {
        static const optics_ui_icon_cmd cmds[] = {
            {0, {1, 1}},      {1, {2, 2}},   {4, {}},      /* "ring"   */
            {5, {12, 12, 3}},                              /* "hub"    */
            {0, {8, 12}},     {1, {11, 15}}, {1, {16, 8}}, /* "tick" */
        };
        optics_ui_icon_run runs[] = {
            {.first_cmd = 0, .count = 3, .color = 0xFF0000FFu, .fill = 0},
            {.first_cmd = 3, .count = 1, .color = 0xFF00FF00u, .fill = 1},
            {.first_cmd = 4, .count = 3, .color = 0xFFFF0000u, .fill = 0},
        };
        optics_ui_icon_desc desc = {
            .cmds = cmds,
            .count = 7,
            .runs = runs,
            .run_count = 3,
        };
        optics_ui__icon_run_span spans[OPTICS_UI_ICON_MAX_RUNS];
        uint32_t n = optics_ui__icon_run_spans(&desc, OPTICS_UI_ICON_RENDER_STROKE, spans,
                                               OPTICS_UI_ICON_MAX_RUNS);
        OT_EXPECT(n == 3);
        if (n == 3) {
            OT_EXPECT(spans[0].first_cmd == 0 && spans[0].count == 3 && !spans[0].fill);
            OT_EXPECT(spans[1].first_cmd == 3 && spans[1].count == 1 && spans[1].fill);
            OT_EXPECT(spans[2].first_cmd == 4 && spans[2].count == 3 && !spans[2].fill);
            /* Distinct colours are preserved, not collapsed to the theme. */
            optics_gfx_color theme = optics_gfx_color_rgba_premul(1, 2, 3, 255);
            OT_EXPECT(optics_ui__icon_run_color(spans[0].color, theme) != theme);
            OT_EXPECT(optics_ui__icon_run_color(spans[1].color, theme) != theme);
            OT_EXPECT(optics_ui__icon_run_color(spans[2].color, theme) != theme);
            OT_EXPECT(optics_ui__icon_run_color(spans[0].color, theme) !=
                      optics_ui__icon_run_color(spans[1].color, theme));
            /* color == 0 means "follow the caller", exactly like a built-in. */
            OT_EXPECT(optics_ui__icon_run_color(0, theme) == theme);
        }

        /* A collapsed descriptor (built-in icon) is exactly one span covering the
         * whole stream, painted in the caller's colour. */
        optics_ui_icon_desc collapsed = {.cmds = cmds, .count = 7};
        n = optics_ui__icon_run_spans(&collapsed, OPTICS_UI_ICON_RENDER_FILL, spans,
                                      OPTICS_UI_ICON_MAX_RUNS);
        OT_EXPECT(n == 1);
        if (n == 1) {
            OT_EXPECT(spans[0].first_cmd == 0 && spans[0].count == 7);
            OT_EXPECT(spans[0].fill);       /* fill mode from the icon table */
            OT_EXPECT(spans[0].color == 0); /* follow the caller's colour */
        }

        /* A run table that overruns the command stream is refused outright, so a
         * corrupt descriptor degrades to "no spans" rather than reading OOB. */
        optics_ui_icon_run bad_runs[] = {{.first_cmd = 6, .count = 99, .color = 0, .fill = 0}};
        optics_ui_icon_desc bad = {.cmds = cmds, .count = 7, .runs = bad_runs, .run_count = 1};
        OT_EXPECT(optics_ui__icon_run_spans(&bad, 0, spans, OPTICS_UI_ICON_MAX_RUNS) == 0);
    }

    return OT_REPORT();
}
