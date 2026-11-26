/*
 * icon_mask.h — baked coverage masks for icons (ADR-0112).
 *
 * An icon is vector geometry: a stream of path commands in a 24×24 box. Drawn
 * directly, that geometry is rasterised at present time through the canvas's
 * attachment MSAA, which carries only `sample_count + 1` coverage levels (5 for
 * the canvas's fixed 4× target). A 14-pixel chevron stroke is therefore
 * quantised to three of those levels and reads as a staircase — the banding is
 * structural, not a bug in the geometry.
 *
 * A mask fixes it at the root: rasterise the icon's geometry once, at its final
 * device size, into a device-free 1x gfx canvas with continuous analytic
 * coverage, and read the resulting coverage plane (`optics_gfx_canvas_cpu_coverage`).
 * That plane carries up to 256 coverage levels per pixel and is resolution-specific,
 * so blitting it back at exactly one texel per device pixel reproduces the vector
 * shape with no resampling.
 *
 * Blit is therefore 1:1 and must stay that way: the quad's device-pixel origin
 * and extent are integers, and the sampler is NEAREST. Anything else reintroduces
 * the filtering blur this design exists to remove.
 *
 * Ownership: masks are cached per (icon, device size, device stroke, paint run)
 * in the ui context, bounded and LRU-evicted. The coverage plane lives either
 * as host bytes (device-less canvas) or as an R8 optics_gfx_image (GPU canvas); the
 * GPU image is retained by any display-list segment that draws the quad
 * (optics_gfx_encoder_draw_glyph_run retains its atlas), so eviction never invalidates
 * a published snapshot.
 */
#ifndef OPTICS_UI_ICON_MASK_H
#define OPTICS_UI_ICON_MASK_H

#include "../internal.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Which layer of an icon a mask holds. The body and its outline are separately
 * coloured (the outline is a distinct paint, drawn under the body), so they must
 * be separately masked: one union mask would force them to share one ink and
 * silently drop the outline colour. */
typedef enum optics_ui__icon_mask_layer : uint32_t {
    OPTICS_UI_ICON_MASK_BODY = 0,   /* fill, or the run's own stroke width        */
    OPTICS_UI_ICON_MASK_OUTLINE = 1 /* stroke widened by the outline width        */
} optics_ui__icon_mask_layer;

/* One icon paint run, resolved from a optics_ui_icon_desc. Runtime SVGs carry an
 * explicit run per source shape (its own colour and fill/stroke mode); a
 * built-in icon is exactly one run covering the whole stream, painted in the
 * caller's colour with the icon table's render mode.
 *
 * Both the mask bake and the vector fallback resolve runs through these, so the
 * two paths can never disagree about how many runs an icon has, where each one
 * starts, or how it is painted — the disagreement that silently discarded every
 * per-shape colour an SVG declared. */
typedef struct optics_ui__icon_run_span {
    uint32_t first_cmd;
    uint32_t count;
    optics_gfx_color color; /* premultiplied 0xAARRGGBB; 0 = follow the caller's colour */
    bool fill;
} optics_ui__icon_run_span;

/* Resolve `desc` into up to `cap` run spans. Returns 0 for a malformed
 * descriptor (an out-of-range run table) or an empty stream. */
uint32_t optics_ui__icon_run_spans(const optics_ui_icon_desc *desc, optics_ui__icon_run_span *out,
                                   uint32_t cap);

/* Append one run's command stream to `p`, scaled by `s` and offset by (ox, oy).
 * The single emitter for icon geometry: the bake and the vector fallback both
 * call it, so they cannot draw different shapes for the same icon. */
void optics_ui__emit_run_path(optics_gfx_path *p, const optics_ui_icon_cmd *cmds, uint32_t first,
                              uint32_t count, float s, float ox, float oy);

/* Resolve one run's colour: a run carrying `color == 0` follows the caller's
 * (theme) colour; otherwise returns the run's premultiplied `optics_gfx_color`. */
optics_gfx_color optics_ui__icon_run_color(optics_gfx_color run_color,
                                           optics_gfx_color theme_color);

/* The coverage mask for one paint run of one icon at one device size. */
typedef struct optics_ui__icon_mask {
    optics_ui_icon_id icon_id;
    uint32_t dev_px;       /* icon box edge, device px                     */
    uint32_t dev_stroke_q; /* body stroke width in 1/16 device px; 0 = fill */
    uint32_t outline_q;    /* outline width in 1/16 device px; 0 = body     */
    uint32_t run_index;    /* paint run index; 0 for collapsed (built-in)   */
    uint8_t layer;         /* optics_ui__icon_mask_layer                        */
    uint8_t pad[3];
    uint32_t halo; /* halo around the icon box, device px          */
    uint32_t w, h; /* full mask extent = dev_px + 2 * halo         */

    /* `host` is always live: row-major 8-bit coverage bytes, used by the
     * CPU backend. `image` is additionally live on a GPU context (R8,
     * NEAREST-sampled), so the exact same compiled display list replays
     * cleanly into both GPU and CPU canvases without branching at draw time.
     * Both are owned by the entry. */
    uint8_t *host;
    optics_gfx_image *image;

    uint64_t last_used; /* LRU tick within the owning pool */
} optics_ui__icon_mask;

/* Allocate the pool and reset it. Called once at context creation;
 * allocation failure is non-fatal (cap stays 0, icons draw the live vector
 * path), so widget correctness never depends on this cache. */
void optics_ui__icon_mask_pool_init(optics_ui *ui);

/* Release every cached mask (images included) and empty the pool. */
void optics_ui__icon_mask_pool_destroy(optics_ui *ui);

/* Evict every cached mask belonging to `icon_id` (called on unregister). */
void optics_ui__icon_mask_evict(optics_ui *ui, optics_ui_icon_id icon_id);

/* Look up — baking on miss — the mask for one LAYER of `icon_id`'s paint run
 * `run_index` at `dev_px` device pixels, with `dev_stroke` device-pixel body
 * stroke width (0 for a fill run) and `outline` device-pixel outline width (0
 * when the icon has none).
 *
 * The two layers are separate masks, not one union: an outline is painted in its
 * own colour beneath the body, so sharing a single coverage map would force both
 * to one ink and drop the outline colour entirely.
 *
 * Returns NULL when the icon is unknown, the geometry is empty, the layer is
 * meaningless (an outline layer with no outline), or the bake/upload fails; the
 * caller then falls back to drawing the vector stream directly, so a failure
 * degrades quality rather than dropping the icon.
 *
 * `dev_stroke` and `outline` are quantised to 1/16 device px inside the key so
 * the pool has bounded cardinality; the quantised values are what the mask is
 * baked with, so key and bytes always agree. */
optics_ui__icon_mask *optics_ui__icon_mask_acquire(optics_ui *ui, optics_ui_icon_id icon_id,
                                                   uint32_t dev_px, float dev_stroke, float outline,
                                                   uint32_t run_index,
                                                   optics_ui__icon_mask_layer layer);

/* Pool lifecycle. init() is called once from optics_ui_create (allocation failure is
 * non-fatal: the pool stays at cap 0 and icons fall back to the live vector
 * path), destroy() from optics_ui_release. */
void optics_ui__icon_mask_pool_init(optics_ui *ui);

/* The NEAREST sampler every mask blit must use (created lazily on the first
 * GPU bake, released with the pool). NULL on a device-less canvas, where the
 * host-coverage blit path has no sampler at all. */
optics_gfx_sampler *optics_ui__icon_mask_sampler(optics_ui *ui);

#ifdef __cplusplus
}
#endif

#endif /* OPTICS_UI_ICON_MASK_H */
