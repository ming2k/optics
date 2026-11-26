/*
 * icon_mask.c — baked device-resolution coverage masks for icons (ADR-0112).
 *
 * Why this exists:
 *
 *   An icon is a vector command stream in a 24x24 box. Drawn live, every frame
 *   would flatten and rasterize each of its paint runs again. A mask does that
 *   ONCE per (icon, device size, stroke): the geometry is rasterized through
 *   the CPU canvas's analytic coverage into a device-resolution coverage plane,
 *   and every later draw is a single 1:1 blit of that plane. The plane is
 *   already at the final device resolution, so the blit needs no resampling or
 *   filtering.
 *
 * The three properties the cache depends on:
 *   - 1:1 blit. The quad origin and extent are integers in device pixels and
 *     the sampler is NEAREST. Fractional placement or LINEAR here would blur
 *     the mask and reintroduce exactly the softness the design removes.
 *   - Per-paint-run white ink. A run's mask holds geometry only; colour comes
 *     from the per-quad tint at draw time (optics_gfx_glyph_quad::color), so one
 *     cached mask serves every theme state, hover tint, and opacity.
 *   - Device scale is in the key. HiDPI (scale 2) bakes at 2× and still blits
 *     1:1, so the mask is as sharp as the display, not upscaled to it.
 */
#include "icon_mask.h"

#include <math.h>
#include <optics/gfx/canvas.h>
#include <optics/gfx/canvas_cpu.h>
#include <optics/gfx/canvas_helpers.h>
#include <optics/gfx/math.h>
#include <optics/gfx/vulkan.h>
#include <stdlib.h>
#include <string.h>

/* Bounded pool. Sized for the realistic working set: a nav rail plus toolbar
 * plus menu (well under 100 distinct icons) at two states/sizes each. Eviction
 * is LRU, and eviction is safe for published snapshots because every recorded
 * glyph-run segment retains the image it draws. The capacity lives in
 * internal.h (OPTICS_UI_ICON_MASK_POOL_CAP) so the pool's inline sizing and this
 * module cannot drift apart. */

/* Halo around the icon box, in device pixels: enough for half the stroke, its
 * round cap, and the analytic fringe. Caps the clamp so a pathological
 * replacement-stroke width cannot explode the mask extent. */
#define OPTICS_UI_ICON_MASK_MAX_PAD 64u

/* Largest device-pixel icon box we will bake. Beyond this the vector path is
 * both cheap and high-resolution enough that the mask buys nothing, and the
 * sample buffer would be large. */
#define OPTICS_UI_ICON_MASK_MAX_PX 256u

void optics_ui__icon_mask_pool_init(optics_ui *ui) {
    if (!ui)
        return;
    ui->icon_masks.cap = OPTICS_UI_ICON_MASK_POOL_CAP;
    ui->icon_masks.tick = 0;
    ui->icon_masks.count = 0;
    ui->icon_masks.sampler = nullptr;
    ui->icon_masks.entries =
        optics_ui__alloc(ui, ui->icon_masks.cap * sizeof(optics_ui__icon_mask));
    if (!ui->icon_masks.entries)
        ui->icon_masks.cap = 0; /* baking degrades to the live-vector fallback */
}

void optics_ui__icon_mask_pool_destroy(optics_ui *ui) {
    if (!ui)
        return;
    for (uint32_t i = 0; i < ui->icon_masks.count; ++i) {
        optics_ui__icon_mask *m = &ui->icon_masks.entries[i];
        optics_ui__free(ui, m->host);
        if (m->image)
            optics_gfx_image_release(m->image);
        *m = (optics_ui__icon_mask){};
    }
    optics_ui__free(ui, ui->icon_masks.entries);
    ui->icon_masks.entries = nullptr;
    ui->icon_masks.count = 0;
    ui->icon_masks.cap = 0;
    if (ui->icon_masks.sampler) {
        optics_gfx_sampler_release(ui->icon_masks.sampler);
        ui->icon_masks.sampler = nullptr;
    }
}

void optics_ui__icon_mask_evict(optics_ui *ui, optics_ui_icon_id icon_id) {
    if (!ui || !ui->icon_masks.entries)
        return;
    uint32_t i = 0;
    while (i < ui->icon_masks.count) {
        optics_ui__icon_mask *m = &ui->icon_masks.entries[i];
        if (m->icon_id == icon_id) {
            optics_ui__free(ui, m->host);
            if (m->image)
                optics_gfx_image_release(m->image);
            uint32_t last = ui->icon_masks.count - 1u;
            if (i != last)
                ui->icon_masks.entries[i] = ui->icon_masks.entries[last];
            ui->icon_masks.entries[last] = (optics_ui__icon_mask){};
            ui->icon_masks.count = last;
        } else {
            ++i;
        }
    }
}

/* NEAREST is not an optimisation here — it is the correctness condition. The
 * mask is baked at exactly the device resolution it is drawn at, so any
 * filtering would blend neighbouring coverage values and soften the edges. */
static optics_gfx_sampler *mask_sampler(optics_ui *ui) {
    if (ui->icon_masks.sampler)
        return ui->icon_masks.sampler;
    if (!ui->device)
        return nullptr;
    optics_gfx_sampler_desc sd = OPTICS_GFX_SAMPLER_DESC_INIT;
    sd.min_filter = OPTICS_GFX_FILTER_NEAREST;
    sd.mag_filter = OPTICS_GFX_FILTER_NEAREST;
    sd.mipmap_mode = OPTICS_GFX_FILTER_NEAREST;
    sd.address_u = OPTICS_GFX_ADDRESS_CLAMP_TO_EDGE;
    sd.address_v = OPTICS_GFX_ADDRESS_CLAMP_TO_EDGE;
    sd.address_w = OPTICS_GFX_ADDRESS_CLAMP_TO_EDGE;
    sd.max_anisotropy = 1.0f;
    optics_gfx_sampler *s = nullptr;
    if (optics_gfx_sampler_create(ui->device, &sd, &s) != OPTICS_GFX_OK)
        return nullptr;
    ui->icon_masks.sampler = s;
    return s;
}

/* Bake one layer of one run into `canvas`. `layer` selects the body (the run's
 * own fill or stroke) or its outline (the same geometry stroked wider by the
 * outline width). The two layers are separate masks because they are painted in
 * different colours — a single union mask would force both to one ink and drop
 * the outline colour.
 *
 * Run geometry itself comes from replay.c's emit_run_path via the span passed
 * in: one emitter, so the bake and the vector fallback can never draw different
 * shapes for the same icon. */
static void bake_run_layer(optics_gfx_canvas *canvas, optics_gfx_arena *arena,
                           const optics_ui_icon_desc *desc, const optics_ui__icon_run_span *span,
                           float scale, float halo, float stroke_dev, float outline_dev,
                           optics_ui__icon_mask_layer layer) {
    optics_gfx_path *p = nullptr;
    if (optics_gfx_path_create(&p, arena) != OPTICS_GFX_OK)
        return;
    optics_ui__emit_run_path(p, desc->cmds, span->first_cmd, span->count, scale, halo, halo);

    optics_gfx_geometry g = {.kind = OPTICS_GFX_GEOM_PATH, .path = {.path = p}};
    if (layer == OPTICS_UI_ICON_MASK_OUTLINE) {
        /* Outline: the body's own width plus the outline on both sides. A fill
         * run has no stroke, so its outline starts from zero. */
        g.stroke.width = (span->fill ? 0.0f : stroke_dev) + outline_dev * 2.0f;
        g.stroke.cap = OPTICS_GFX_CAP_ROUND;
        g.stroke.join = OPTICS_GFX_JOIN_ROUND;
    } else if (!span->fill) {
        g.stroke.width = stroke_dev;
        g.stroke.cap = OPTICS_GFX_CAP_ROUND;
        g.stroke.join = OPTICS_GFX_JOIN_ROUND;
    }
    /* White ink: a mask stores coverage only, and the draw-time per-quad tint
     * supplies the colour. Anything else would bake widget state into the cache. */
    optics_gfx_brush b = optics_gfx_brush_solid(optics_gfx_color_rgba_premul(255, 255, 255, 255));
    optics_gfx_canvas_draw_geometry(
        canvas, &(optics_gfx_canvas_draw_geometry_opts){.geom = &g, .brush = &b});
}

uint32_t optics_ui__icon_run_spans(const optics_ui_icon_desc *desc, optics_ui__icon_run_span *out,
                                   uint32_t cap) {
    if (!desc || !desc->cmds || desc->count == 0 || !out || cap == 0)
        return 0;
    if (desc->runs && desc->run_count > 0) {
        uint32_t n = desc->run_count < cap ? desc->run_count : cap;
        for (uint32_t i = 0; i < n; ++i) {
            const optics_ui_icon_run *r = &desc->runs[i];
            if (r->first_cmd > desc->count || r->count > desc->count - r->first_cmd)
                return 0;
            out[i] = (optics_ui__icon_run_span){
                .first_cmd = r->first_cmd,
                .count = r->count,
                .color = r->color,
                .fill = r->paint == OPTICS_UI_ICON_PAINT_FILL,
            };
        }
        return n;
    }
    out[0] = (optics_ui__icon_run_span){
        .first_cmd = 0,
        .count = desc->count,
        .color = 0,
        .fill = desc->paint == OPTICS_UI_ICON_PAINT_FILL,
    };
    return 1;
}

void optics_ui__emit_run_path(optics_gfx_path *p, const optics_ui_icon_cmd *cmds, uint32_t first,
                              uint32_t count, float s, float ox, float oy) {
    for (uint32_t i = first; i < first + count; ++i) {
        const optics_ui_icon_cmd *cmd = &cmds[i];
        const float *pp = cmd->params;
        switch (cmd->kind) {
        case OPTICS_UI_ICON_CMD_MOVE:
            optics_gfx_path_move_to(p, pp[0] * s + ox, pp[1] * s + oy);
            break;
        case OPTICS_UI_ICON_CMD_LINE:
            optics_gfx_path_line_to(p, pp[0] * s + ox, pp[1] * s + oy);
            break;
        case OPTICS_UI_ICON_CMD_CUBIC:
            optics_gfx_path_cubic_to(p, pp[0] * s + ox, pp[1] * s + oy, pp[2] * s + ox,
                                     pp[3] * s + oy, pp[4] * s + ox, pp[5] * s + oy);
            break;
        case OPTICS_UI_ICON_CMD_QUAD:
            optics_gfx_path_quad_to(p, pp[0] * s + ox, pp[1] * s + oy, pp[2] * s + ox,
                                    pp[3] * s + oy);
            break;
        case OPTICS_UI_ICON_CMD_CLOSE:
            optics_gfx_path_close(p);
            break;
        case OPTICS_UI_ICON_CMD_CIRCLE:
            optics_gfx_path_add_circle(p, pp[0] * s + ox, pp[1] * s + oy, pp[2] * s);
            break;
        case OPTICS_UI_ICON_CMD_RECT:
            optics_gfx_path_add_rect(
                p, (optics_gfx_rect){pp[0] * s + ox, pp[1] * s + oy, pp[2] * s, pp[3] * s});
            break;
        default:
            break;
        }
    }
}

optics_gfx_color optics_ui__icon_run_color(optics_gfx_color run_color,
                                           optics_gfx_color theme_color) {
    return run_color != 0 ? run_color : theme_color;
}

static optics_ui__icon_mask *pool_find(optics_ui *ui, optics_ui_icon_id icon_id, uint32_t dev_px,
                                       uint32_t dev_stroke_q, uint32_t outline_q,
                                       uint32_t run_index, optics_ui__icon_mask_layer layer) {
    for (uint32_t i = 0; i < ui->icon_masks.count; ++i) {
        optics_ui__icon_mask *m = &ui->icon_masks.entries[i];
        if (m->icon_id == icon_id && m->dev_px == dev_px && m->dev_stroke_q == dev_stroke_q &&
            m->outline_q == outline_q && m->run_index == run_index && m->layer == (uint8_t)layer)
            return m;
    }
    return nullptr;
}

/* Open a slot for a new mask, evicting the least recently used entry when the
 * pool is full. Returns NULL when the pool has no storage. */
static optics_ui__icon_mask *pool_open(optics_ui *ui) {
    if (ui->icon_masks.cap == 0)
        return nullptr;
    if (ui->icon_masks.count < ui->icon_masks.cap)
        return &ui->icon_masks.entries[ui->icon_masks.count++];

    uint32_t victim = 0;
    for (uint32_t i = 1; i < ui->icon_masks.count; ++i) {
        if (ui->icon_masks.entries[i].last_used < ui->icon_masks.entries[victim].last_used)
            victim = i;
    }
    optics_ui__icon_mask *m = &ui->icon_masks.entries[victim];
    optics_ui__free(ui, m->host);
    if (m->image)
        optics_gfx_image_release(m->image);
    *m = (optics_ui__icon_mask){};
    return m;
}

static uint32_t quantize_16(float v) {
    if (v <= 0.0f)
        return 0u;
    uint32_t q = (uint32_t)(v * 16.0f + 0.5f);
    return q > UINT16_MAX ? UINT16_MAX : q;
}

optics_ui__icon_mask *optics_ui__icon_mask_acquire(optics_ui *ui, optics_ui_icon_id icon_id,
                                                   uint32_t dev_px, float dev_stroke, float outline,
                                                   uint32_t run_index,
                                                   optics_ui__icon_mask_layer layer) {
    if (!ui || dev_px == 0 || dev_px > OPTICS_UI_ICON_MASK_MAX_PX || ui->icon_masks.cap == 0)
        return nullptr;

    const optics_ui_icon_desc *desc = optics_ui__icon_desc(ui, icon_id);
    optics_ui__icon_run_span spans[OPTICS_UI_ICON_MAX_RUNS];
    uint32_t span_count = optics_ui__icon_run_spans(desc, spans, OPTICS_UI_ICON_MAX_RUNS);
    if (run_index >= span_count)
        return nullptr;
    const optics_ui__icon_run_span *run = &spans[run_index];
    if (layer == OPTICS_UI_ICON_MASK_OUTLINE && !(outline > 0.0f))
        return nullptr; /* asking for an outline layer with no outline */

    /* Quantise stroke and outline into the key so the pool has bounded
     * cardinality and the baked geometry always matches the key it is filed
     * under. A fill run has no stroke, so it keys on 0 regardless of the width
     * the caller passed — otherwise the same fill would be baked again for every
     * size that happens to change the (unused) stroke value. The same holds for
     * a body-layer mask, which never carries the outline. */
    uint32_t dev_stroke_q = run->fill ? 0u : quantize_16(dev_stroke);
    float stroke_dev = (float)dev_stroke_q / 16.0f;
    if (!run->fill && stroke_dev <= 0.0f)
        return nullptr; /* a stroke run with no width draws nothing */
    uint32_t outline_q = (layer == OPTICS_UI_ICON_MASK_OUTLINE) ? quantize_16(outline) : 0u;
    float outline_dev = (float)outline_q / 16.0f;

    uint64_t tick = ++ui->icon_masks.tick;
    optics_ui__icon_mask *hit =
        pool_find(ui, icon_id, dev_px, dev_stroke_q, outline_q, run_index, layer);
    if (hit) {
        hit->last_used = tick;
        return hit;
    }

    /* Halo covers the widest ink this layer can lay down: half the body stroke
     * plus the full outline for the outline layer, half the body stroke for the
     * body layer, plus the AA fringe. */
    float widest = run->fill ? 0.0f : stroke_dev;
    if (layer == OPTICS_UI_ICON_MASK_OUTLINE)
        widest += outline_dev * 2.0f;
    uint32_t halo = (uint32_t)(widest * 0.5f + 1.999f);
    if (halo < 1u)
        halo = 1u;
    if (halo > OPTICS_UI_ICON_MASK_MAX_PAD)
        halo = OPTICS_UI_ICON_MASK_MAX_PAD;
    uint32_t extent = dev_px + 2u * halo;
    if (extent == 0 || extent > 4096u)
        return nullptr;

    /* Bake at device resolution with 1x continuous analytic coverage. Content
     * scale is 1.0 because the canvas is created at `extent` physical pixels
     * and the geometry is placed in device pixels directly — there is no logical
     * transform to compound, which is what makes the result device-sized. */
    optics_gfx_canvas *canvas = nullptr;
    if (optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = extent,
                                                           .height = extent},
                                 &canvas) != OPTICS_GFX_OK)
        return nullptr;

    optics_gfx_arena arena;
    if (optics_gfx_arena_init(&arena, 64u * 1024u, nullptr) != OPTICS_GFX_OK) {
        optics_gfx_canvas_release(canvas);
        return nullptr;
    }

    optics_gfx_color transparent = {};
    bool baked = false;
    if (optics_gfx_canvas_begin(
            canvas, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                   .clear_color = &transparent}) == OPTICS_GFX_OK) {
        const float s = (float)dev_px / 24.0f;
        bake_run_layer(canvas, &arena, desc, run, s, (float)halo, stroke_dev, outline_dev, layer);
        baked = optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK;
    }
    optics_gfx_arena_deinit(&arena);

    if (!baked) {
        optics_gfx_canvas_release(canvas);
        return nullptr;
    }

    uint32_t w = 0, h = 0, stride = 0;
    const uint8_t *cov = optics_gfx_canvas_cpu_coverage(canvas, &w, &h, &stride);
    if (!cov || w != extent || h != extent) {
        optics_gfx_canvas_release(canvas);
        return nullptr;
    }

    optics_ui__icon_mask *m = pool_open(ui);
    if (!m) {
        optics_gfx_canvas_release(canvas);
        return nullptr;
    }

    /* The coverage plane is width-strided; pack it into row-major bytes. Host
     * bytes are kept unconditionally: an icon mask is a few hundred bytes, so
     * retaining them in RAM allows the same compiled display list to replay into
     * both GPU canvases and CPU canvases (headless snapshots, offscreen export)
     * without branching at the call site or baking twice. */
    uint8_t *host_plane = optics_ui__alloc(ui, (size_t)w * h);
    if (!host_plane) {
        ui->icon_masks.count--;
        optics_gfx_canvas_release(canvas);
        return nullptr;
    }
    for (uint32_t y = 0; y < h; ++y)
        memcpy(host_plane + (size_t)y * w, cov + (size_t)y * stride, w);

    optics_gfx_image *img = nullptr;
    if (ui->device) {
        /* GPU canvas: create and upload an R8 image so GPU passes can bind
         * and sample it via Vulkan bindless descriptors. */
        optics_gfx_image_desc id = OPTICS_GFX_IMAGE_DESC_INIT;
        id.width = w;
        id.height = h;
        id.format = OPTICS_GFX_FORMAT_R8_UNORM;
        if (optics_gfx_image_create(ui->device, &id, &img) != OPTICS_GFX_OK) {
            optics_ui__free(ui, host_plane);
            ui->icon_masks.count--;
            optics_gfx_canvas_release(canvas);
            return nullptr;
        }
        optics_gfx_result up =
            optics_gfx_image_update_region(img, &(optics_gfx_image_update_region_opts){
                                                    .width = w,
                                                    .height = h,
                                                    .data = host_plane,
                                                    .bytes = (size_t)w * h,
                                                });
        if (up != OPTICS_GFX_OK) {
            optics_gfx_image_release(img);
            optics_ui__free(ui, host_plane);
            ui->icon_masks.count--;
            optics_gfx_canvas_release(canvas);
            return nullptr;
        }
    }

    m->host = host_plane;
    m->image = img;

    m->icon_id = icon_id;
    m->dev_px = dev_px;
    m->dev_stroke_q = dev_stroke_q;
    m->outline_q = outline_q;
    m->run_index = run_index;
    m->layer = (uint8_t)layer;
    m->halo = halo;
    m->w = w;
    m->h = h;
    m->last_used = tick;

    optics_gfx_canvas_release(canvas);
    return m;
}

optics_gfx_sampler *optics_ui__icon_mask_sampler(optics_ui *ui) {
    return mask_sampler(ui);
}
