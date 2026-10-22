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
#include "../../../libs/lens/src/internal.h"
#include "../../../libs/lens/src/render/icon_mask.h"
#include "test_helpers.h"
#include <flux/canvas.h>
#include <flux/canvas_cpu.h>
#include <lens/icon.h>
#include <math.h>

/* Count distinct partial-coverage values in a rendered icon's alpha channel. */
static int icon_partial_levels(lens_icon_id id, float size) {
    lens *ui = nullptr;
    if (lens_create(&(lens_desc){.scale = 1.0f}, &ui) != FLUX_OK)
        return -1;
    const uint32_t w = 32, h = 32;
    lens_input in = {.display_size = {(float)w, (float)h}, .dt_seconds = 0.016f};
    lens_begin(ui, &in);
    lens_icon(ui, &(lens_icon_opts){.id = id, .size = size});
    lens_end(ui);

    flux_canvas *c = nullptr;
    if (flux_canvas_create_cpu(w, h, 1.0f, &c) != FLUX_OK) {
        lens_release(ui);
        return -1;
    }
    flux_color clear = {0};
    if (flux_canvas_begin(c, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                      .clear_color = &clear}) == FLUX_OK) {
        lens_scene_snapshot *s = nullptr;
        if (lens_snapshot_create(ui, &s) == FLUX_OK) {
            (void)lens_snapshot_submit(s, c);
            lens_snapshot_release(s);
        }
        (void)flux_canvas_end(c);
    }
    uint32_t pw = 0, ph = 0, st = 0;
    const uint8_t *px = flux_canvas_cpu_pixels(c, &pw, &ph, &st);
    int seen[256] = {0};
    if (px) {
        for (uint32_t y = 0; y < ph; ++y)
            for (uint32_t x = 0; x < pw; ++x)
                seen[px[(size_t)y * st + (size_t)x * 4 + 3]] = 1;
    }
    int levels = 0;
    for (int i = 1; i < 255; ++i)
        levels += seen[i];
    flux_canvas_release(c);
    lens_release(ui);
    return levels;
}

/* Coverage-weighted centroid of an icon rendered into a `box` box at `scale`. */
static bool icon_centroid(lens_icon_id id, float size, float scale, uint32_t box, double *out_cx,
                          double *out_cy) {
    lens *ui = nullptr;
    if (lens_create(&(lens_desc){.scale = scale}, &ui) != FLUX_OK)
        return false;
    uint32_t w = (uint32_t)(box * scale), h = w;
    lens_input in = {.display_size = {(float)box, (float)box}, .dt_seconds = 0.016f};
    lens_begin(ui, &in);
    lens_icon(ui, &(lens_icon_opts){.id = id, .size = size});
    lens_end(ui);

    /* Content scale stays 1.0: lens already applies ui->scale internally, so a
     * scaled canvas here would apply it twice. */
    flux_canvas *c = nullptr;
    if (flux_canvas_create_cpu(w, h, 1.0f, &c) != FLUX_OK) {
        lens_release(ui);
        return false;
    }
    flux_color clear = {0};
    if (flux_canvas_begin(c, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                      .clear_color = &clear}) == FLUX_OK) {
        lens_scene_snapshot *s = nullptr;
        if (lens_snapshot_create(ui, &s) == FLUX_OK) {
            (void)lens_snapshot_submit(s, c);
            lens_snapshot_release(s);
        }
        (void)flux_canvas_end(c);
    }
    uint32_t pw = 0, ph = 0, st = 0;
    const uint8_t *px = flux_canvas_cpu_pixels(c, &pw, &ph, &st);
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
    flux_canvas_release(c);
    lens_release(ui);
    return ok;
}

/* alert_circle's ink is symmetric about the 24-unit box centre, so its centroid
 * is the box centre regardless of stroke details. */
static void check_centred_at_scale(float scale) {
    const float size = 14.0f;
    uint32_t box = 24;
    double cx = 0, cy = 0;
    CHECK(icon_centroid(LENS_ICON_ALERT_CIRCLE, size, scale, box, &cx, &cy));
    /* The icon node fills `size` logical px at the layout origin; its centre is
     * size*scale/2 in device px, minus half a pixel for pixel-centre indexing. */
    double expect = (double)size * scale * 0.5 - 0.5;
    CHECK(fabs(cx - expect) < 0.6);
    CHECK(fabs(cy - expect) < 0.6);
}

int main(void) {
    /* ---- mask cache: baked at device size, keyed by device stroke ---- */
    {
        lens *ui = nullptr;
        CHECK(lens_create(&(lens_desc){.scale = 1.0f}, &ui) == FLUX_OK);

        float stroke = 2.0f * 20.0f / 24.0f;
        lensi_icon_mask *a = lensi_icon_mask_acquire(ui, LENS_ICON_CHECK, 20u, stroke, 0.0f, 0u,
                                                     LENSI_ICON_MASK_BODY);
        CHECK(a != nullptr);
        if (a) {
            CHECK(a->dev_px == 20u);
            CHECK(a->w == 20u + 2u * a->halo);
            CHECK(a->h == a->w);
            /* Device-less lens: the mask lives as host bytes, not an image. */
            CHECK(a->host != nullptr);
            CHECK(a->image == nullptr);
            int nonzero = 0;
            for (uint32_t i = 0; i < a->w * a->h; ++i)
                if (a->host[i])
                    nonzero++;
            CHECK(nonzero > 0);
            /* Full-coverage ink must exist: a stroke thinner than one pixel
             * would top out below 255 and read as a permanently grey glyph. */
            int peak = 0;
            for (uint32_t i = 0; i < a->w * a->h; ++i)
                if (a->host[i] > peak)
                    peak = a->host[i];
            CHECK(peak == 255);
        }

        /* Same request returns the same entry — no duplicate bakes. */
        lensi_icon_mask *again = lensi_icon_mask_acquire(ui, LENS_ICON_CHECK, 20u, stroke, 0.0f, 0u,
                                                         LENSI_ICON_MASK_BODY);
        CHECK(again == a);

        /* A different device size is a different entry. */
        lensi_icon_mask *bigger = lensi_icon_mask_acquire(ui, LENS_ICON_CHECK, 24u, stroke, 0.0f,
                                                          0u, LENSI_ICON_MASK_BODY);
        CHECK(bigger != nullptr && bigger != a);
        if (bigger)
            CHECK(bigger->dev_px == 24u);

        /* An outline layer is its own entry, and an outline layer with no
         * outline width is meaningless rather than silently a body mask. */
        lensi_icon_mask *outline = lensi_icon_mask_acquire(ui, LENS_ICON_CHECK, 20u, stroke, 1.5f,
                                                           0u, LENSI_ICON_MASK_OUTLINE);
        CHECK(outline != nullptr && outline != a);
        CHECK(lensi_icon_mask_acquire(ui, LENS_ICON_CHECK, 20u, stroke, 0.0f, 0u,
                                      LENSI_ICON_MASK_OUTLINE) == nullptr);

        /* A body-layer mask never carries the outline, so asking with an outline
         * width must not mint a second entry with byte-identical coverage. */
        lensi_icon_mask *body_dup = lensi_icon_mask_acquire(ui, LENS_ICON_CHECK, 20u, stroke, 1.5f,
                                                            0u, LENSI_ICON_MASK_BODY);
        CHECK(body_dup == a);

        /* An unknown run index and a zero device size are rejected, so a caller
         * falls back instead of masking nothing. */
        CHECK(lensi_icon_mask_acquire(ui, LENS_ICON_CHECK, 20u, stroke, 0.0f, 7u,
                                      LENSI_ICON_MASK_BODY) == nullptr);
        CHECK(lensi_icon_mask_acquire(ui, LENS_ICON_CHECK, 0u, stroke, 0.0f, 0u,
                                      LENSI_ICON_MASK_BODY) == nullptr);

        lens_release(ui); /* must free every cached mask without leaking */
    }

    /* ---- rendered icons carry far more coverage than 4x MSAA allows ---- */
    {
        /* 4x MSAA can express at most 3 partial levels. A 14 px stroked icon
         * through the mask path must clear that comfortably. */
        int chevron = icon_partial_levels(LENS_ICON_CHEVRON_RIGHT, 14.0f);
        int circle = icon_partial_levels(LENS_ICON_ALERT_CIRCLE, 14.0f);
        CHECK(chevron > 3);
        CHECK(circle > 3);
        int star = icon_partial_levels(LENS_ICON_STAR_ROUNDED_FILLED, 20.0f);
        CHECK(star > 3);
    }

    /* ---- placement is device-grid exact at every scale ---- */
    check_centred_at_scale(1.0f);
    check_centred_at_scale(1.25f);
    check_centred_at_scale(1.5f);
    check_centred_at_scale(2.0f);

    /* ---- filled (non-stroke) icons bake and render ---- */
    {
        int filled = icon_partial_levels(LENS_ICON_STAR_ROUNDED_FILLED, 16.0f);
        int outline = icon_partial_levels(LENS_ICON_STAR_ROUNDED, 16.0f);
        CHECK(filled > 0);
        CHECK(outline > 0);
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
        static const lens_icon_cmd cmds[] = {
            {0, {1, 1}},      {1, {2, 2}},   {4, {0}},     /* "ring"   */
            {5, {12, 12, 3}},                              /* "hub"    */
            {0, {8, 12}},     {1, {11, 15}}, {1, {16, 8}}, /* "tick" */
        };
        lens_icon_run runs[] = {
            {.first_cmd = 0, .count = 3, .color = 0xFF0000FFu, .fill = 0},
            {.first_cmd = 3, .count = 1, .color = 0xFF00FF00u, .fill = 1},
            {.first_cmd = 4, .count = 3, .color = 0xFFFF0000u, .fill = 0},
        };
        lens_icon_desc desc = {
            .cmds = cmds,
            .count = 7,
            .runs = runs,
            .run_count = 3,
        };
        lensi_icon_run_span spans[LENSI_ICON_MAX_RUNS];
        uint32_t n =
            lensi_icon_run_spans(&desc, LENSI_ICON_RENDER_STROKE, spans, LENSI_ICON_MAX_RUNS);
        CHECK(n == 3);
        if (n == 3) {
            CHECK(spans[0].first_cmd == 0 && spans[0].count == 3 && !spans[0].fill);
            CHECK(spans[1].first_cmd == 3 && spans[1].count == 1 && spans[1].fill);
            CHECK(spans[2].first_cmd == 4 && spans[2].count == 3 && !spans[2].fill);
            /* Distinct colours are preserved, not collapsed to the theme. */
            flux_color theme = flux_color_rgba_premul(1, 2, 3, 255);
            CHECK(lensi_icon_run_color(spans[0].color, theme) != theme);
            CHECK(lensi_icon_run_color(spans[1].color, theme) != theme);
            CHECK(lensi_icon_run_color(spans[2].color, theme) != theme);
            CHECK(lensi_icon_run_color(spans[0].color, theme) !=
                  lensi_icon_run_color(spans[1].color, theme));
            /* color == 0 means "follow the caller", exactly like a built-in. */
            CHECK(lensi_icon_run_color(0, theme) == theme);
        }

        /* A collapsed descriptor (built-in icon) is exactly one span covering the
         * whole stream, painted in the caller's colour. */
        lens_icon_desc collapsed = {.cmds = cmds, .count = 7};
        n = lensi_icon_run_spans(&collapsed, LENSI_ICON_RENDER_FILL, spans, LENSI_ICON_MAX_RUNS);
        CHECK(n == 1);
        if (n == 1) {
            CHECK(spans[0].first_cmd == 0 && spans[0].count == 7);
            CHECK(spans[0].fill);       /* fill mode from the icon table */
            CHECK(spans[0].color == 0); /* follow the caller's colour */
        }

        /* A run table that overruns the command stream is refused outright, so a
         * corrupt descriptor degrades to "no spans" rather than reading OOB. */
        lens_icon_run bad_runs[] = {{.first_cmd = 6, .count = 99, .color = 0, .fill = 0}};
        lens_icon_desc bad = {.cmds = cmds, .count = 7, .runs = bad_runs, .run_count = 1};
        CHECK(lensi_icon_run_spans(&bad, 0, spans, LENSI_ICON_MAX_RUNS) == 0);
    }

    return TEST_REPORT();
}
