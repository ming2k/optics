/*
 * icon_mask.c — baked device-resolution coverage masks for icons (ADR-0112).
 *
 * Why this exists, precisely:
 *
 *   An icon is a vector command stream in a 24×24 box. Drawn live, it is
 *   rasterised through the canvas's attachment MSAA, which the canvas pins at
 *   4× (FLUX_CANVAS_SAMPLES). A resolved 4× pixel can only carry coverage in
 *   {0, ¼, ½, ¾, 1} — five levels, three of them partial. A 14 px icon with the
 *   standard 2/24 stroke is a 1.17 px line; its whole edge budget is one or two
 *   pixels, so it lands on two of those levels and reads as a staircase. Adding
 *   MSAA samples shifts the problem but never removes it: the banding is
 *   quantisation of coverage, and it is baked into every frame.
 *
 *   A mask removes it at the source. The geometry is rasterised ONCE per
 *   (icon, device size, stroke) into a device-free canvas supersampled 8×8 per
 *   pixel, and the resolved coverage plane is kept. That plane carries 65
 *   levels per pixel — visually continuous — and is already at the final
 *   device resolution, so the blit is 1:1: no resampling, no filtering, no
 *   dependence on the draw-time MSAA budget.
 *
 * The three consequences that make this correct rather than a workaround:
 *   - 1:1 blit. The quad origin and extent are integers in device pixels and
 *     the sampler is NEAREST. Fractional placement or LINEAR here would blur
 *     the mask and reintroduce exactly the softness the design removes.
 *   - Per-paint-run white ink. A run's mask holds geometry only; colour comes
 *     from the per-quad tint at draw time (flux_glyph_quad::color), so one
 *     cached mask serves every theme state, hover tint, and opacity.
 *   - Device scale is in the key. HiDPI (scale 2) bakes at 2× and still blits
 *     1:1, so the mask is as sharp as the display, not upscaled to it.
 */
#include "icon_mask.h"

#include <flux/canvas.h>
#include <flux/canvas_cpu.h>
#include <flux/canvas_helpers.h>
#include <flux/math.h>
#include <flux/vulkan.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Coverage resolution for the bake. 8×8 = 64 samples/pixel = 65 coverage
 * levels, which is beyond what 8-bit output can distinguish on an edge. The
 * bake is a one-off per cache entry, so the supersample cost is paid once —
 * a 24 px icon bakes into a 192×192 sample buffer (~600 KiB float RGBA). */
#define LENSI_ICON_MASK_SUPERSAMPLE 8u

/* Bounded pool. Sized for the realistic working set: a nav rail plus toolbar
 * plus menu (well under 100 distinct icons) at two states/sizes each. Eviction
 * is LRU, and eviction is safe for published snapshots because every recorded
 * glyph-run segment retains the image it draws. The capacity lives in
 * internal.h (LENSI_ICON_MASK_POOL_CAP) so the pool's inline sizing and this
 * module cannot drift apart. */

/* Halo around the icon box, in device pixels: enough for half the stroke, its
 * round cap, and the analytic fringe. Caps the clamp so a pathological
 * replacement-stroke width cannot explode the mask extent. */
#define LENSI_ICON_MASK_MAX_PAD 64u

/* Largest device-pixel icon box we will bake. Beyond this the vector path is
 * both cheap and high-resolution enough that the mask buys nothing, and the
 * sample buffer would be large. */
#define LENSI_ICON_MASK_MAX_PX 256u

void lensi_icon_mask_pool_init(lens *ui) {
    if (!ui)
        return;
    ui->icon_masks.cap = LENSI_ICON_MASK_POOL_CAP;
    ui->icon_masks.tick = 0;
    ui->icon_masks.count = 0;
    ui->icon_masks.sampler = nullptr;
    ui->icon_masks.entries = calloc(ui->icon_masks.cap, sizeof(lensi_icon_mask));
    if (!ui->icon_masks.entries)
        ui->icon_masks.cap = 0; /* baking degrades to the live-vector fallback */
}

void lensi_icon_mask_pool_destroy(lens *ui) {
    if (!ui)
        return;
    for (uint32_t i = 0; i < ui->icon_masks.count; ++i) {
        lensi_icon_mask *m = &ui->icon_masks.entries[i];
        free(m->host);
        if (m->image)
            flux_image_release(m->image);
        *m = (lensi_icon_mask){0};
    }
    free(ui->icon_masks.entries);
    ui->icon_masks.entries = nullptr;
    ui->icon_masks.count = 0;
    ui->icon_masks.cap = 0;
    if (ui->icon_masks.sampler) {
        flux_sampler_release(ui->icon_masks.sampler);
        ui->icon_masks.sampler = nullptr;
    }
}

/* NEAREST is not an optimisation here — it is the correctness condition. The
 * mask is baked at exactly the device resolution it is drawn at, so any
 * filtering would blend neighbouring coverage values and soften the edges. */
static flux_sampler *mask_sampler(lens *ui) {
    if (ui->icon_masks.sampler)
        return ui->icon_masks.sampler;
    if (!ui->device)
        return nullptr;
    flux_sampler_desc sd = FLUX_SAMPLER_DESC_INIT;
    sd.min_filter = FLUX_FILTER_NEAREST;
    sd.mag_filter = FLUX_FILTER_NEAREST;
    sd.mipmap_mode = FLUX_FILTER_NEAREST;
    sd.address_u = FLUX_ADDRESS_CLAMP_TO_EDGE;
    sd.address_v = FLUX_ADDRESS_CLAMP_TO_EDGE;
    sd.address_w = FLUX_ADDRESS_CLAMP_TO_EDGE;
    sd.max_anisotropy = 1.0f;
    flux_sampler *s = nullptr;
    if (flux_sampler_create(ui->device, &sd, &s) != FLUX_OK)
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
static void bake_run_layer(flux_canvas *canvas, flux_arena *arena, const lens_icon_desc *desc,
                           const lensi_icon_run_span *span, float scale, float halo,
                           float stroke_dev, float outline_dev, lensi_icon_mask_layer layer) {
    flux_path *p = nullptr;
    if (flux_path_create(&p, arena) != FLUX_OK)
        return;
    lensi_emit_run_path(p, desc->cmds, span->first_cmd, span->count, scale, halo, halo);

    flux_geometry g = {.kind = FLUX_GEOM_PATH, .path = {.path = p}};
    if (layer == LENSI_ICON_MASK_OUTLINE) {
        /* Outline: the body's own width plus the outline on both sides. A fill
         * run has no stroke, so its outline starts from zero. */
        g.stroke.width = (span->fill ? 0.0f : stroke_dev) + outline_dev * 2.0f;
        g.stroke.cap = FLUX_CAP_ROUND;
        g.stroke.join = FLUX_JOIN_ROUND;
    } else if (!span->fill) {
        g.stroke.width = stroke_dev;
        g.stroke.cap = FLUX_CAP_ROUND;
        g.stroke.join = FLUX_JOIN_ROUND;
    }
    /* White ink: a mask stores coverage only, and the draw-time per-quad tint
     * supplies the colour. Anything else would bake widget state into the cache. */
    flux_brush b = flux_brush_solid(flux_color_rgba_premul(255, 255, 255, 255));
    flux_canvas_draw_geometry(canvas, &g, &b);
}

uint32_t lensi_icon_run_spans(const lens_icon_desc *desc, uint8_t mode, lensi_icon_run_span *out,
                              uint32_t cap) {
    if (!desc || !desc->cmds || desc->count == 0 || !out || cap == 0)
        return 0;
    if (desc->runs && desc->run_count > 0) {
        uint32_t n = desc->run_count < cap ? desc->run_count : cap;
        for (uint32_t i = 0; i < n; ++i) {
            const lens_icon_run *r = &desc->runs[i];
            if (r->first_cmd > desc->count || r->count > desc->count - r->first_cmd)
                return 0;
            out[i] = (lensi_icon_run_span){
                .first_cmd = r->first_cmd,
                .count = r->count,
                .color = r->color,
                .fill = r->fill != 0,
            };
        }
        return n;
    }
    out[0] = (lensi_icon_run_span){
        .first_cmd = 0,
        .count = desc->count,
        .color = 0,
        .fill = mode == LENSI_ICON_RENDER_FILL,
    };
    return 1;
}

void lensi_emit_run_path(flux_path *p, const lens_icon_cmd *cmds, uint32_t first, uint32_t count,
                         float s, float ox, float oy) {
    for (uint32_t i = first; i < first + count; ++i) {
        const lens_icon_cmd *cmd = &cmds[i];
        const float *pp = cmd->params;
        switch (cmd->type) {
        case 0:
            flux_path_move_to(p, pp[0] * s + ox, pp[1] * s + oy);
            break;
        case 1:
            flux_path_line_to(p, pp[0] * s + ox, pp[1] * s + oy);
            break;
        case 2:
            flux_path_cubic_to(p, pp[0] * s + ox, pp[1] * s + oy, pp[2] * s + ox, pp[3] * s + oy,
                               pp[4] * s + ox, pp[5] * s + oy);
            break;
        case 3:
            flux_path_quad_to(p, pp[0] * s + ox, pp[1] * s + oy, pp[2] * s + ox, pp[3] * s + oy);
            break;
        case 4:
            flux_path_close(p);
            break;
        case 5:
            flux_path_add_circle(p, pp[0] * s + ox, pp[1] * s + oy, pp[2] * s);
            break;
        case 6:
            flux_path_add_rect(p,
                               (flux_rect){pp[0] * s + ox, pp[1] * s + oy, pp[2] * s, pp[3] * s});
            break;
        default:
            break;
        }
    }
}

flux_color lensi_icon_run_color(uint32_t run_color, flux_color theme_color) {
    if (run_color == 0)
        return theme_color;
    uint8_t r = (uint8_t)(run_color >> 16);
    uint8_t g = (uint8_t)(run_color >> 8);
    uint8_t b = (uint8_t)run_color;
    uint8_t a = (uint8_t)(run_color >> 24);
    return flux_color_rgba_premul(r, g, b, a);
}

static lensi_icon_mask *pool_find(lens *ui, int32_t icon_id, uint32_t dev_px, uint32_t dev_stroke_q,
                                  uint32_t outline_q, uint32_t run_index,
                                  lensi_icon_mask_layer layer) {
    for (uint32_t i = 0; i < ui->icon_masks.count; ++i) {
        lensi_icon_mask *m = &ui->icon_masks.entries[i];
        if (m->icon_id == icon_id && m->dev_px == dev_px && m->dev_stroke_q == dev_stroke_q &&
            m->outline_q == outline_q && m->run_index == run_index && m->layer == (uint8_t)layer)
            return m;
    }
    return nullptr;
}

/* Open a slot for a new mask, evicting the least recently used entry when the
 * pool is full. Returns NULL when the pool has no storage. */
static lensi_icon_mask *pool_open(lens *ui) {
    if (ui->icon_masks.cap == 0)
        return nullptr;
    if (ui->icon_masks.count < ui->icon_masks.cap)
        return &ui->icon_masks.entries[ui->icon_masks.count++];

    uint32_t victim = 0;
    for (uint32_t i = 1; i < ui->icon_masks.count; ++i) {
        if (ui->icon_masks.entries[i].last_used < ui->icon_masks.entries[victim].last_used)
            victim = i;
    }
    lensi_icon_mask *m = &ui->icon_masks.entries[victim];
    free(m->host);
    if (m->image)
        flux_image_release(m->image);
    *m = (lensi_icon_mask){0};
    return m;
}

static uint32_t quantize_16(float v) {
    if (v <= 0.0f)
        return 0u;
    uint32_t q = (uint32_t)(v * 16.0f + 0.5f);
    return q > UINT16_MAX ? UINT16_MAX : q;
}

lensi_icon_mask *lensi_icon_mask_acquire(lens *ui, int32_t icon_id, uint32_t dev_px,
                                         float dev_stroke, float outline, uint32_t run_index,
                                         lensi_icon_mask_layer layer) {
    if (!ui || dev_px == 0 || dev_px > LENSI_ICON_MASK_MAX_PX || ui->icon_masks.cap == 0)
        return nullptr;

    const lens_icon_desc *desc = lensi_icon_desc(icon_id);
    lensi_icon_run_span spans[LENSI_ICON_MAX_RUNS];
    uint32_t span_count =
        lensi_icon_run_spans(desc, lensi_icon_mode(icon_id), spans, LENSI_ICON_MAX_RUNS);
    if (run_index >= span_count)
        return nullptr;
    const lensi_icon_run_span *run = &spans[run_index];
    if (layer == LENSI_ICON_MASK_OUTLINE && !(outline > 0.0f))
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
    uint32_t outline_q = (layer == LENSI_ICON_MASK_OUTLINE) ? quantize_16(outline) : 0u;
    float outline_dev = (float)outline_q / 16.0f;

    uint64_t tick = ++ui->icon_masks.tick;
    lensi_icon_mask *hit =
        pool_find(ui, icon_id, dev_px, dev_stroke_q, outline_q, run_index, layer);
    if (hit) {
        hit->last_used = tick;
        return hit;
    }

    /* Halo covers the widest ink this layer can lay down: half the body stroke
     * plus the full outline for the outline layer, half the body stroke for the
     * body layer, plus the AA fringe. */
    float widest = run->fill ? 0.0f : stroke_dev;
    if (layer == LENSI_ICON_MASK_OUTLINE)
        widest += outline_dev * 2.0f;
    uint32_t halo = (uint32_t)(widest * 0.5f + 1.999f);
    if (halo < 1u)
        halo = 1u;
    if (halo > LENSI_ICON_MASK_MAX_PAD)
        halo = LENSI_ICON_MASK_MAX_PAD;
    uint32_t extent = dev_px + 2u * halo;
    if (extent == 0 || extent > 4096u)
        return nullptr;

    /* Bake at device resolution with 8x8 coverage. Content scale is 1.0 because
     * the canvas is created at `extent` physical pixels and the geometry is
     * placed in device pixels directly — there is no logical transform to
     * compound, which is what makes the result exactly device-sized. */
    flux_canvas *canvas = nullptr;
    if (flux_canvas_create_cpu_ss(extent, extent, 1.0f, LENSI_ICON_MASK_SUPERSAMPLE, &canvas) !=
        FLUX_OK)
        return nullptr;

    flux_arena arena;
    if (flux_arena_init(&arena, 64u * 1024u, nullptr) != FLUX_OK) {
        flux_canvas_release(canvas);
        return nullptr;
    }

    flux_color transparent = {0};
    bool baked = false;
    if (flux_canvas_begin(canvas, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                           .clear_color = &transparent}) ==
        FLUX_OK) {
        const float s = (float)dev_px / 24.0f;
        bake_run_layer(canvas, &arena, desc, run, s, (float)halo, stroke_dev, outline_dev, layer);
        baked = flux_canvas_end(canvas) == FLUX_OK;
    }
    flux_arena_deinit(&arena);

    if (!baked) {
        flux_canvas_release(canvas);
        return nullptr;
    }

    uint32_t w = 0, h = 0, stride = 0;
    const uint8_t *cov = flux_canvas_cpu_coverage(canvas, &w, &h, &stride);
    if (!cov || w != extent || h != extent) {
        flux_canvas_release(canvas);
        return nullptr;
    }

    lensi_icon_mask *m = pool_open(ui);
    if (!m) {
        flux_canvas_release(canvas);
        return nullptr;
    }

    /* The coverage plane is width-strided; pack it into row-major bytes. Host
     * bytes are kept unconditionally: an icon mask is a few hundred bytes, so
     * retaining them in RAM allows the same compiled display list to replay into
     * both GPU canvases and CPU canvases (headless snapshots, offscreen export)
     * without branching at the call site or baking twice. */
    uint8_t *host_plane = malloc((size_t)w * h);
    if (!host_plane) {
        ui->icon_masks.count--;
        flux_canvas_release(canvas);
        return nullptr;
    }
    for (uint32_t y = 0; y < h; ++y)
        memcpy(host_plane + (size_t)y * w, cov + (size_t)y * stride, w);

    flux_image *img = nullptr;
    if (ui->device) {
        /* GPU canvas: create and upload an R8 image so GPU passes can bind
         * and sample it via Vulkan bindless descriptors. */
        flux_image_desc id = FLUX_IMAGE_DESC_INIT;
        id.width = w;
        id.height = h;
        id.format = FLUX_FORMAT_R8_UNORM;
        if (flux_image_create(ui->device, &id, &img) != FLUX_OK) {
            free(host_plane);
            ui->icon_masks.count--;
            flux_canvas_release(canvas);
            return nullptr;
        }
        flux_result up = flux_image_update_region(img, 0, 0, w, h, host_plane, (size_t)w * h);
        if (up != FLUX_OK) {
            flux_image_release(img);
            free(host_plane);
            ui->icon_masks.count--;
            flux_canvas_release(canvas);
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

    flux_canvas_release(canvas);
    return m;
}

flux_sampler *lensi_icon_mask_sampler(lens *ui) {
    return mask_sampler(ui);
}
