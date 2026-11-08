/* context.c — lens lifecycle and the per-frame envelope (ADR-0024/0009). */

#include "../internal.h"
#include "../render/icon_mask.h"
#include <stdatomic.h>

void lens_version(int *major, int *minor, int *patch) {
    if (major)
        *major = LENS_VERSION_MAJOR;
    if (minor)
        *minor = LENS_VERSION_MINOR;
    if (patch)
        *patch = LENS_VERSION_PATCH;
}

uint32_t lens_version_number(void) {
    return LENS_VERSION_NUMBER;
}

bool lens_version_check(int major, int minor, int patch) {
    if (major != LENS_VERSION_MAJOR)
        return false;
    if (minor > LENS_VERSION_MINOR)
        return false;
    if (minor == LENS_VERSION_MINOR && patch > LENS_VERSION_PATCH)
        return false;
    return true;
}

const char *lens_version_string(void) {
    return LENS_STRINGIFY(LENS_VERSION_MAJOR) "." LENS_STRINGIFY(
        LENS_VERSION_MINOR) "." LENS_STRINGIFY(LENS_VERSION_PATCH);
}

/* The one lens allocation funnel (ADR-0129, ADR-0135).
 *
 * The ledger row is now written by flux_device_alloc itself, with lens's own
 * (owner, kind) pair — previously this function counted by hand *and* called
 * the device allocator, so attribution lived in two places. The device-less
 * fallback keeps plain malloc: a lens context created without a device (CPU-only
 * layout, unit tests) has no ledger to write to, and that is a legitimate state,
 * not a violation. */
void *lensi_alloc(lens *ui, size_t bytes) {
    if (!ui->device)
        return malloc(bytes); // RAW_ALLOC_OK(no-device)
    void *p = nullptr;
    if (flux_device_alloc(ui->device,
                          &(flux_device_alloc_opts){
                              .type = FLUX_TYPE_DEVICE_ALLOC_OPTS,
                              .bytes = bytes,
                              .owner = "lens/core",
                              .kind = "host",
                              .scope = FLUX_SCOPE_CONTEXT,
                          },
                          &p) != FLUX_OK)
        return nullptr;
    return p;
}

/* Device-scoped allocation pair for owners that outlive their lens context
 * (currently: scene snapshots, whose last release can come from a host after
 * lens_release). Same attribution as lensi_alloc, but keyed on the device the
 * memory belongs to rather than on a context that may be gone. */
void *lensi_alloc_by_device(flux_device *d, size_t bytes) {
    /* Zeroed on both paths: flux_device_alloc guarantees it, and the device-less
     * fallback must match, because callers rely on the zeroing (a snapshot
     * struct's display_list/node_count start life as zero). Returning unzeroed
     * malloc here was a real bug — it turned a zeroed struct into garbage. */
    if (!d)
        return calloc(1, bytes); // RAW_ALLOC_OK(no-device)
    void *p = nullptr;
    if (flux_device_alloc(d,
                          &(flux_device_alloc_opts){
                              .type = FLUX_TYPE_DEVICE_ALLOC_OPTS,
                              .bytes = bytes,
                              .owner = "lens/core",
                              .kind = "snapshot",
                              .scope = FLUX_SCOPE_CONTEXT,
                          },
                          &p) != FLUX_OK)
        return nullptr;
    return p;
}

void lensi_free_by_device(flux_device *d, void *ptr) {
    if (!ptr)
        return;
    if (!d) {
        free(ptr); // RAW_ALLOC_OK(no-device)
        return;
    }
    flux_device_free(d, &(flux_device_free_opts){
                            .type = FLUX_TYPE_DEVICE_FREE_OPTS,
                            .ptr = ptr,
                            .owner = "lens/core",
                            .kind = "snapshot",
                            .scope = FLUX_SCOPE_CONTEXT,
                        });
}

void lensi_free(lens *ui, void *ptr) {
    if (!ptr)
        return;
    if (!ui->device) {
        free(ptr); // RAW_ALLOC_OK(no-device)
        return;
    }
    flux_device_free(ui->device, &(flux_device_free_opts){
                                     .type = FLUX_TYPE_DEVICE_FREE_OPTS,
                                     .ptr = ptr,
                                     .owner = "lens/core",
                                     .kind = "host",
                                     .scope = FLUX_SCOPE_CONTEXT,
                                 });
}

static void lensi_theme_normalize(lens_theme *t);

/* Size-aware theme copy (ADR-0032 decision 1 — previously documented but
 * not implemented, which made it an over-read on input and an over-write
 * on output for callers built against different lens_theme layouts).
 * Semantics match the implemented lens_input guard in lens_begin():
 *   size == 0        -> legacy caller: trust the full struct (pre-guard ABI)
 *   0 < size < lib   -> copy only the caller's prefix, default the rest
 *   size > lib size  -> copy only the library's prefix (caller is newer)
 * In every case the stored theme ends with size = sizeof(lens_theme) as
 * the library now sees it. */
static lens_theme lensi_theme_copy_in(const lens_theme *src) {
    lens_theme t = lens_theme_default();
    if (!src)
        return t;
    size_t n = src->size ? src->size : sizeof(lens_theme);
    if (n > sizeof(lens_theme))
        n = sizeof(lens_theme);
    memcpy(&t, src, n);
    /* The library's notion of the layout wins from here on. */
    t.size = sizeof(lens_theme);
    return t;
}

/* Size-aware lens_desc read (mirrors the lens_input guard in lens_begin):
 * read only min(caller, library) bytes so a caller compiled against a
 * different lens_desc layout — older or newer — cannot make the library
 * read past its own struct. Returns a fully library-local copy. */
static lens_desc lensi_desc_copy_in(const lens_desc *desc) {
    lens_desc d;
    memset(&d, 0, sizeof d);
    if (!desc)
        return d;
    size_t n = desc->size ? desc->size : sizeof(lens_desc);
    if (n > sizeof(lens_desc))
        n = sizeof(lens_desc);
    memcpy(&d, desc, n);
    /* Sanitize: the guard itself is the library's, never the caller's. */
    d.size = sizeof(lens_desc);
    return d;
}

flux_result lens_create(const lens_desc *desc, lens **out) {
    if (!out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    *out = NULL;

    lens *ui = calloc(1, sizeof *ui); // RAW_ALLOC_OK(no-device)
    if (!ui)
        return FLUX_ERROR_OUT_OF_MEMORY;

    /* Read the caller's descriptor through the size guard: a caller built
     * against a different lens_desc layout never makes the library read
     * past its own struct. All field reads below come from this local
     * copy, never from `desc` directly. (The parameter re-point is what
     * the dead-store check flags; the parameter itself is never read
     * again after this line — drop the re-point and read the copy `d`
     * directly.) */
    const lens_desc d = lensi_desc_copy_in(desc);

    static atomic_uint_fast64_t next_identity = 1;
    ui->identity = atomic_fetch_add_explicit(&next_identity, 1, memory_order_relaxed);
    ui->device = d.device;
    if (ui->device) {
        flux_device *dev = flux_device_retain(ui->device);
        (void)dev;
    }

    ui->theme = d.theme.size ? lensi_theme_copy_in(&d.theme) : lens_theme_default();
    lensi_theme_normalize(&ui->theme);
    ui->scale = (d.scale > 0.0f) ? d.scale : 1.0f;
    ui->text_scale = (d.text_scale > 0.0f && isfinite(d.text_scale)) ? d.text_scale : 1.0f;
    ui->opacity = 1.0f;
    ui->tooltip.opacity = 1.0f;
    ui->clipboard = d.clipboard;
    ui->dnd_host = d.dnd;

    /* Caret blink clock defaults (ADR-0151). 530 ms is the platform
     * convention every toolkit converged on; the clock starts visible so the
     * first frame after focus shows a caret rather than a gap. The clock is
     * inactive until a *widget* claims the caret (`lensi_set_caret_rect`): a
     * host-owned caret has its own clock in the app, and driving it from here
     * as well would make lens report flips the app's painter never made. */
    ui->caret_blink_enabled = true;
    ui->caret_lens_owned = false;
    ui->caret_blink_period_s =
        d.caret_blink_period_s > 0.0f ? d.caret_blink_period_s : LENSI_CARET_BLINK_PERIOD_S;
    ui->caret_phase_s = 0.0f;
    ui->caret_visible = true;
    ui->caret_presented_visible = true;

    size_t arena_bytes = d.arena_bytes ? d.arena_bytes : LENSI_DEFAULT_ARENA_BYTES;
    flux_result r = flux_arena_init(&ui->arena, arena_bytes, NULL);
    if (r != FLUX_OK)
        goto fail_arena;

    uint32_t cap = d.store_capacity ? d.store_capacity : LENSI_DEFAULT_STORE_CAP;
    r = lensi_store_init(ui, cap);
    if (r != FLUX_OK)
        goto fail_store;

    /* Shared text engine (glyph). Always usable: it degrades to
     * monospace metrics internally when no shaping backend or font is
     * available, so widgets never branch on backend presence. Only fails
     * on allocation, which is non-fatal here (text simply stays blank). */
    if (glyph_create(&(glyph_desc){.device = ui->device, .scale = ui->scale}, &ui->text) != FLUX_OK)
        ui->text = NULL;

    /* Icon coverage-mask cache (ADR-0112). Allocation failure is non-fatal:
     * the pool stays at cap 0 and icons draw through the live vector path. */
    lensi_icon_mask_pool_init(ui);

    *out = ui;
    return FLUX_OK;

fail_store:
    flux_arena_deinit(&ui->arena);
fail_arena:
    if (ui->device)
        flux_device_release(ui->device);
    free(ui); // RAW_ALLOC_OK(no-device)
    return r;
}

void lens_release(lens *ui) {
    if (!ui)
        return;
    lens_snapshot_release(ui->presented_snapshot);
    lensi_icon_mask_pool_destroy(ui);
    lensi_ghost_destroy(ui);
    glyph_release(ui->text); /* null-safe */
    lensi_store_destroy(ui);
    flux_arena_deinit(&ui->arena);
    /* Context scope drain point (ADR-0129). Emitted while the device is
     * still alive so the report can name rows; the device is released
     * afterwards. A non-zero row here is a lens-owned leak, which is
     * exactly what had no attribution at all before this point. */
    if (ui->device) {
        flux_observe_report_emit(ui->device, FLUX_SCOPE_CONTEXT);
        flux_device_release(ui->device);
    }
    free(ui); // RAW_ALLOC_OK(no-device)
}

static void lensi_theme_normalize(lens_theme *t) {
    if (t->font_size_title <= 0.0f)
        t->font_size_title = t->font_size * (24.0f / 14.0f);
    if (t->font_size_h1 <= 0.0f)
        t->font_size_h1 = t->font_size * (20.0f / 14.0f);
    if (t->font_size_h2 <= 0.0f)
        t->font_size_h2 = t->font_size * (16.0f / 14.0f);
    if (t->font_size_h3 <= 0.0f)
        t->font_size_h3 = t->font_size;
    if (t->font_weight <= 0.0f)
        t->font_weight = 400.0f;
    if (t->font_weight_bold <= 0.0f)
        t->font_weight_bold = 700.0f;
    if (t->scrollbar_width <= 0.0f)
        t->scrollbar_width = 8.0f;
    if (t->scrollbar_radius < 0.0f)
        t->scrollbar_radius = t->scrollbar_width * 0.5f;
    if (t->scrollbar_min_thumb_h <= 0.0f)
        t->scrollbar_min_thumb_h = 28.0f;
    /* Normalize semantic surface tokens (ADR-0105) */
    if (!t->color_surface)
        t->color_surface = t->color_bg;
    if (!t->color_surface_sunken)
        t->color_surface_sunken = t->color_hover ? t->color_hover : t->color_bg;
    if (!t->color_surface_elevated)
        t->color_surface_elevated = t->color_active ? t->color_active : t->color_bg;

    if (!t->color_slider_track)
        t->color_slider_track = t->color_surface_sunken;
    if (!t->color_slider_fill)
        t->color_slider_fill = t->color_accent;
    if (!t->color_slider_knob)
        t->color_slider_knob = t->color_fg;
    if (t->slider_track_thickness <= 0.0f)
        t->slider_track_thickness = 6.0f;
    if (t->slider_knob_size <= 0.0f)
        t->slider_knob_size = 14.0f;

    /* Normalize material recipes if uninitialized (e.g. from zero-initialized or older callers) */
    if (t->materials.foundation.plate_opacity <= 0.0f && !t->materials.foundation.fallback) {
        uint8_t r = 0, g = 0, b = 0, a = 0;
        if (t->color_bg)
            flux_color_unpack(t->color_bg, &r, &g, &b, &a);
        bool dark = ((int)r + g + b) < 384;
        t->materials.foundation = lens_material_recipe_default(LENS_MATERIAL_MICA, dark);
        t->materials.command = lens_material_recipe_default(LENS_MATERIAL_MICA_ALT, dark);
        t->materials.floating = lens_material_recipe_default(LENS_MATERIAL_ACRYLIC, dark);
        t->materials.lens_body = lens_material_recipe_default(LENS_MATERIAL_LIQUID_GLASS, dark);
    }
}

void lens_set_text_family(lens *ui, lens_text_family family) {
    if (!ui)
        return;
    ui->text_family = (int)family;
}

lens_text_family lens_get_text_family(const lens *ui) {
    return ui ? (lens_text_family)ui->text_family : LENS_TEXT_FAMILY_DEFAULT;
}

void lens_set_theme(lens *ui, lens_theme theme) {
    if (!ui)
        return;
    ui->theme = theme.size ? lensi_theme_copy_in(&theme) : lens_theme_default();
    lensi_theme_normalize(&ui->theme);
}

lens_theme lens_get_theme(const lens *ui) {
    /* By-value return cannot clamp against the caller's layout (the
     * library cannot know it), so the guard works the other way: the
     * caller copies the prefix it knows and must consult .size. The
     * returned struct always carries the library's current
     * sizeof(lens_theme) so a mismatch is detectable rather than silent. */
    lens_theme t = ui ? ui->theme : lens_theme_default();
    t.size = sizeof(lens_theme);
    return t;
}

void lens_set_opacity(lens *ui, float opacity) {
    if (!ui)
        return;
    if (!isfinite(opacity) || opacity < 0.0f)
        opacity = 0.0f;
    else if (opacity > 1.0f)
        opacity = 1.0f;
    ui->opacity = opacity;
}

/* Ghost replay (ADR-0078): the public pin. */
void lens_set_ghost(lens *ui, lens_id subtree_root, float alpha) {
    if (!isfinite(alpha) || alpha < 0.0f)
        alpha = 0.0f;
    else if (alpha > 1.0f)
        alpha = 1.0f;
    lensi_ghost_pin(ui, subtree_root, alpha);
}

float lens_opacity(const lens *ui) {
    return ui ? ui->opacity : 1.0f;
}

void lens_set_scale(lens *ui, float scale) {
    if (ui && isfinite(scale) && scale > 0.0f) {
        ui->scale = scale;
        glyph_set_scale(ui->text, scale); /* null-safe */
    }
}

/* Accessibility text scale — see lens.h for the contract. Applied at the
 * resolved-style funnel (style.c) and the explicit-size entry points, so
 * measure/paint/caret stay in agreement with no further invalidation
 * plumbing: font sizes already ride the draw-command hash (drawlist.c),
 * so a factor change re-keys records automatically. */
void lens_set_text_scale(lens *ui, float factor) {
    if (ui && factor > 0.0f && isfinite(factor))
        ui->text_scale = factor;
}

float lens_text_scale(const lens *ui) {
    return ui ? ui->text_scale : 1.0f;
}

float lens_scale(const lens *ui) {
    return ui ? ui->scale : 1.0f;
}
float lens_dt(const lens *ui) {
    return ui ? ui->input.dt_seconds : 0.0f;
}

bool lens_overflowed(const lens *ui) {
    return ui && ui->overflow;
}
bool lens_has_duplicate_ids(const lens *ui) {
    return ui && ui->duplicate_ids;
}
bool lens_anim_pending(const lens *ui) {
    return ui && ui->anim_pending;
}

/* ------------------------------------------------------------------ */
/*  Caret blink clock (ADR-0151)                                       */
/* ------------------------------------------------------------------ */

/* Advance the blink phase by this frame's delta. Called once per build,
 * before any widget runs, so every consumer in the frame — the skin's
 * caret draw, the repaint query, and the damage walk — reads the same
 * phase.
 *
 * The clock advances unconditionally, not only while a caret exists: the
 * phase is what makes a repaint differ from the last one, and a host that
 * skips frames must be able to resume on the phase it would have reached.
 * `dt_seconds` is the host's frame delta, which is the compositor's pulse
 * interval on a paced frame and the idle tick on a skipped one; that is
 * exactly the cadence a blink should be sampled at. */
static void lensi_caret_advance_blink(lens *ui) {
    if (!ui->caret_blink_enabled || !(ui->caret_blink_period_s > 0.0f))
        return;
    float dt = ui->input.dt_seconds;
    if (!(dt > 0.0f) || !isfinite(dt))
        dt = 0.0f;
    const float period = ui->caret_blink_period_s;
    /* Two periods is the longest delta that can still be integrated as a
     * blink; a host that reports a nonsense delta must not be able to
     * fast-forward the caret through whole cycles in one step. */
    if (dt > 2.0f * period)
        dt = 2.0f * period;
    ui->caret_phase_s = fmodf(ui->caret_phase_s + dt, period);
    ui->caret_visible = ui->caret_phase_s < period * 0.5f;
}

/* True when the caret's current phase differs from the phase actually on
 * screen. This is the *only* caret term `lens_frame_needs_repaint` may
 * consult: a caret that exists but has not flipped is not damage.
 *
 * A host-owned caret is excluded outright (ADR-0151): the app paints it with
 * its own clock, so lens has no authority to declare its phase changed. */
static bool lensi_caret_phase_changed(const lens *ui) {
    if (!ui->caret_lens_owned)
        return false;
    if (ui->caret_rect.w <= 0.0f || ui->caret_rect.h <= 0.0f)
        return false;
    return ui->caret_visible != ui->caret_presented_visible;
}

bool lens_caret_flip_pending(const lens *ui) {
    return ui ? lensi_caret_phase_changed(ui) : false;
}

bool lens_caret_clock_live(const lens *ui) {
    if (!ui)
        return false;
    /* Only a widget-owned caret has a lens clock to advance (ADR-0151). */
    if (!ui->caret_lens_owned)
        return false;
    /* An IME composition holds the caret solid, so no clock is needed. */
    if (ui->input.preedit_utf8[0] != '\0')
        return false;
    if (!ui->caret_blink_enabled)
        return false;
    return ui->caret_rect.w > 0.0f && ui->caret_rect.h > 0.0f;
}

/* Read-only repaint query for damage-driven hosts (valid between lens_end
 * and the next lens_begin). True when anything the last snapshot publication would
 * have produced differs from what is already on screen. */
bool lens_frame_needs_repaint(const lens *ui) {
    if (!ui)
        return false;

    /* Base tree damage: lifecycle, geometry, hover/active eases and
     * draw-list hash changes, rolled up bottom-up by lensi_mark_dirty. */
    if (ui->root &&
        (ui->root->subtree_changed || ui->root->visual_revision != ui->root->presented_revision))
        return true;

    /* Placed subtrees (popups, menus, chrome, backdrops) carry their own
     * subtree_changed (their damage deliberately does not roll up into the
     * root's display-list record — replay.c), so the query consults the
     * band buckets directly. An open/close alters the parent's child
     * sequence, which the base-tree check above already catches. */
    for (uint32_t b = 0; b < (uint32_t)LENS_BAND_COUNT; b++)
        for (uint32_t i = 0; i < ui->band_counts[b]; i++)
            if (ui->bands[b][i] &&
                (ui->bands[b][i]->subtree_changed ||
                 ui->bands[b][i]->visual_revision != ui->bands[b][i]->presented_revision))
                return true;

    /* Tooltip visuals are captured separately from widget nodes. Repaint
     * while active, or when active
     * last frame and now gone (its pixels must be erased). */
    if (ui->tooltip.active || ui->prev_tooltip_active)
        return true;

    /* Time-driven state: an eased value (hover/active fade, slide) still
     * in transit — the next frame differs from this one even with no
     * input. */
    if (ui->anim_pending)
        return true;

    /* Ghosts are time-driven by contract (ADR-0078 decision 6): a fade
     * host re-pins alpha every frame and the countdown ticks every
     * lens_end, so while any ghost exists the next frame differs from the
     * last painted one even when no other damage source is live. Without
     * this term a ghost-only fade stalls the moment the base tree is
     * clean — the leaving surface freezes mid-fade. */
    if (lensi_ghost_active(ui))
        return true;

    /* The caret's *flip*, not its existence (ADR-0151). The old term was
     * `ui->caret_rect.w > 0.0f`, which is a constant for as long as any text
     * widget holds focus: it made `needs_repaint` permanently true, so every
     * host with a focused field could never reach quiescence, and every frame
     * it did paint re-painted an identical caret because no phase existed to
     * advance. A blink is a 2-state clock sampled at the display cadence; only
     * the transition between the two states changes pixels. */
    if (lensi_caret_phase_changed(ui))
        return true;

    return false;
}

static flux_rect rect_union_bounds(flux_rect a, flux_rect b) {
    if (a.w <= 0.0f || a.h <= 0.0f)
        return b;
    if (b.w <= 0.0f || b.h <= 0.0f)
        return a;
    float x0 = fminf(a.x, b.x);
    float y0 = fminf(a.y, b.y);
    float x1 = fmaxf(a.x + a.w, b.x + b.w);
    float y1 = fmaxf(a.y + a.h, b.y + b.h);
    return (flux_rect){x0, y0, x1 - x0, y1 - y0};
}

static flux_rect rect_intersect_bounds(flux_rect a, flux_rect b) {
    float x = fmaxf(a.x, b.x);
    float y = fmaxf(a.y, b.y);
    float r = fminf(a.x + a.w, b.x + b.w);
    float btm = fminf(a.y + a.h, b.y + b.h);
    if (r <= x || btm <= y)
        return (flux_rect){0, 0, 0, 0};
    return (flux_rect){x, y, r - x, btm - y};
}

/* Pixels one node's OWN paint covers this frame: the union of its draw commands
 * resolved against its box, exactly as replay rasterises them.
 *
 * Deliberately not `final_rect`. A label in a stretch column is laid out across
 * the column's full width while its glyphs occupy a few dozen pixels, and the
 * root's box is the entire display — reporting layout boxes is what turned a
 * one-cell terminal edit into a whole-output composite. Command geometry is the
 * only bound that describes what actually changed.
 *
 * Two command kinds are ignored: `CLIP_PUSH`/`CLIP_POP` paint nothing themselves,
 * and including them would re-widen every clipped subtree to its clip rect. A
 * node with no commands has no own paint and contributes nothing here; its
 * descendants are visited separately. */
/* Where one command paints, in absolute coordinates, resolved against `box`. */
static flux_rect paint_of_cmd(flux_rect box, const lens_draw_cmd *c) {
    return lensi_cmd_paint_extent(box, c);
}

static flux_rect node_paint_bounds(const lens_node *n) {
    return lensi_node_paint_bounds(n);
}

/* The same paint, resolved against `box` instead of the node's current
 * `final_rect`. Used only while a node's own command list is still alive (the
 * live walk); a departed node must use its persisted `render_paint_rect`
 * instead, because its command list is arena memory that has been recycled. */
static flux_rect node_paint_bounds_at(const lens_node *n, flux_rect box) {
    flux_rect d = {0, 0, 0, 0};
    if (!n || !n->cmds)
        return box;
    for (uint32_t i = 0; i < n->cmd_count; i++) {
        const lens_draw_cmd *c = &n->cmds[i];
        if (c->kind == LENS_DRAW_CLIP_PUSH || c->kind == LENS_DRAW_CLIP_POP)
            continue;
        d = rect_union_bounds(d, paint_of_cmd(box, c));
    }
    return d.w > 0.0f && d.h > 0.0f ? d : box;
}

/* ---- regional damage ------------------------------------------------------
 *
 * lens_damage_rects reports the frame's damage as a bounded rectangle list. The
 * walk is deliberately not a bounding-box accumulator: a host that repaints
 * regionally (and a compositor hands the list straight to its per-plane damage
 * hint) must never see a HUD chip and a Dock spring merged into one nearly
 * full-output box. A host whose presentation API takes one rectangle collapses
 * the list with `flux_rect_union` at that boundary — never before.
 *
 * Two rules carry the localization:
 *
 *   1. An ABS node owns its whole subtree. A placed band is drawn as one body;
 *      anything inside it that changed changes pixels the band owns.
 *   2. A FLOW node owns only its own paint delta. Its `subtree_changed` rolls
 *      up from every flow descendant, and its `final_rect` is the container's
 *      layout box — for the root that is the entire display. Rolling up here
 *      would report the display for a single edited label.
 */

typedef struct damage_accum {
    /* Fixed capacity: the damage set is bounded by the public contract, so the
     * walk never allocates. `rects == nullptr` asks for the count only. */
    flux_rect *rects;
    uint32_t capacity;
    uint32_t count;  /* rectangles accepted, including any that did not fit */
    bool overflowed; /* at least one rectangle did not fit in `rects` */
} damage_accum;

/* Append a viewport-clipped, pixel-snapped rectangle. */
static void damage_push(damage_accum *acc, const lens *ui, flux_rect rect) {
    if (rect.w <= 0.0f || rect.h <= 0.0f)
        return;
    if (ui->input.display_size.x > 0.0f && ui->input.display_size.y > 0.0f) {
        const flux_rect viewport = {0, 0, ui->input.display_size.x, ui->input.display_size.y};
        rect = rect_intersect_bounds(rect, viewport);
        if (rect.w <= 0.0f || rect.h <= 0.0f)
            return;
    }
    /* Snap outward to whole logical pixels: the host maps these through its
     * output scale, and a fractional edge would round in whichever direction
     * that mapping happened to choose. */
    const float x1 = ceilf(rect.x + rect.w);
    const float y1 = ceilf(rect.y + rect.h);
    rect.x = floorf(rect.x);
    rect.y = floorf(rect.y);
    rect.w = x1 - rect.x;
    rect.h = y1 - rect.y;
    if (rect.w <= 0.0f || rect.h <= 0.0f)
        return;

    if (acc->rects && acc->count < acc->capacity)
        acc->rects[acc->count] = rect;
    else if (acc->rects)
        acc->overflowed = true;
    acc->count++;
}

/* Drop rectangles fully contained in another, then sort into a stable
 * top-left-first order. Overlap is *safe* (a scissored repaint is idempotent and
 * over-covering never leaves stale pixels); this pass exists so repeated or
 * nested reports of the same region — the common shape of an animation tick —
 * do not multiply the list a host has to walk.
 *
 * Reads `in` and writes `out`, which must not alias: compacting in place would
 * let a later candidate's containment test run against an already-overwritten
 * slot, and a candidate wrongly judged "covered" is a damage rectangle silently
 * dropped from the frame. */
static uint32_t damage_normalize(const flux_rect *in, uint32_t count, flux_rect *out) {
    uint32_t kept = 0;
    for (uint32_t i = 0; i < count; i++) {
        const flux_rect candidate = in[i];
        bool covered = false;
        for (uint32_t j = 0; j < count && !covered; j++) {
            if (i == j)
                continue;
            const flux_rect other = in[j];
            if (other.w <= 0.0f || other.h <= 0.0f)
                continue;
            const bool contains = candidate.x >= other.x && candidate.y >= other.y &&
                                  candidate.x + candidate.w <= other.x + other.w &&
                                  candidate.y + candidate.h <= other.y + other.h;
            /* Strict containment, plus equality broken by index so exactly one
             * of two identical rects survives. */
            if (contains && (other.w > candidate.w || other.h > candidate.h || j < i))
                covered = true;
        }
        if (!covered)
            out[kept++] = candidate;
    }
    for (uint32_t i = 1; i < kept; i++) {
        const flux_rect key = out[i];
        uint32_t j = i;
        while (j > 0 && (out[j - 1].y > key.y || (out[j - 1].y == key.y && out[j - 1].x > key.x))) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = key;
    }
    return kept;
}

/* The single rectangle covering `count` rectangles of `in`, or a zero rect when
 * the set is empty. */
static flux_rect damage_bbox(const flux_rect *in, uint32_t count) {
    flux_rect bbox = {0, 0, 0, 0};
    for (uint32_t i = 0; i < count; i++)
        bbox = rect_union_bounds(bbox, in[i]);
    return bbox;
}

static void damage_walk(const lens *ui, const lens_node *n, bool owned, damage_accum *acc) {
    if (!n)
        return;
    /* Past an ABS node the whole subtree is owned by that body: its pixels are
     * the body's pixels, so every changed descendant belongs in the damage. */
    if (n->place == LENS_PLACE_ABS)
        owned = true;

    /* An owned body repaints as a unit, so every changed descendant belongs in
     * the damage; it is reported through its own paint bounds below.
     *
     * A FLOW node contributes only its own paint delta. `visual_revision` is
     * never consulted here: it rolls up from descendants, and the root's own
     * rect is the entire display, so drift is an owned-body question only. */
    const bool changed = owned ? (n->subtree_changed || n->visual_revision != n->presented_revision)
                               : n->self_changed;
    if (changed) {
        damage_push(acc, ui, node_paint_bounds(n));
        /* `render_rect` is what the activated snapshot last drew for this node.
         * It differs from `final_rect` exactly when the node moved or resized,
         * and the pixels it vacated still hold the old paint. */
        if (n->has_render_rect && n->render_rect.w > 0.0f && n->render_rect.h > 0.0f &&
            (n->render_rect.x != n->final_rect.x || n->render_rect.y != n->final_rect.y ||
             n->render_rect.w != n->final_rect.w || n->render_rect.h != n->final_rect.h))
            damage_push(acc, ui, node_paint_bounds_at(n, n->render_rect));

        /* A clip that changed and a placed node that re-composited both change
         * pixels this node does not own: the clip's old band held descendants that
         * are now hidden, and a re-stacked node changed what it occludes. Neither
         * is derivable from the node's own paint, so the regions are reported from
         * the state they changed away from. */
        if (n->restacked) {
            /* The whole placed body, since a re-stacked node can occlude or reveal
             * any part of it, plus the extent the previous stacking showed. */
            damage_push(acc, ui, node_paint_bounds(n));
            if (n->has_render_rect)
                damage_push(acc, ui,
                            n->render_paint_rect.w > 0.0f ? n->render_paint_rect : n->render_rect);
        } else if (n->clip_changed && n->has_prev_clip) {
            /* The clip's *previous* extent, which is where pixels the new clip
             * excludes still show the last presented frame, unioned with the new
             * one so a widening clip repaints what it reveals. */
            damage_push(acc, ui, n->prev_clip);
            damage_push(acc, ui, lensi_node_child_clip(n));
        }
    }
    for (const lens_node *c = n->first_child; c; c = c->next_sibling)
        damage_walk(ui, c, owned, acc);
}

/* Nodes that left the child lists still own pixels on screen.
 *
 * `lensi_store_reap` keeps a node in the store for LENSI_LEAVE_GRACE_FRAMES and
 * only then frees it, but it detaches the node from its parent's sibling list
 * immediately. `damage_walk` traverses `first_child`/`next_sibling`, so it can
 * never reach such a node — while the geometry the last activated snapshot
 * recorded for it still describes the pixels that have to be erased. Before
 * ADR-0143 this was covered by the (grossly over-wide) whole-display fallback;
 * once damage was localized the omission left the vacated band frozen on screen
 * until an unrelated full repaint crossed it.
 *
 * Only PERSISTENT geometry is read here, never `n->cmds`:
 *
 *   - `render_rect` is a value copied out of the snapshot at activation
 *     (`lens_snapshot_activate`), so it is valid on any later frame.
 *   - `n->cmds` and the strings they point at live in the per-frame arena
 *     (`lens_begin` calls `flux_arena_reset`, which only rewinds the cursor and
 *     recycles immediately). A LEAVING node is never touched, so
 *     `lensi_node_reset_frame` never clears its `cmds` — the pointer survives
 *     while the memory behind it is handed to *this* frame's nodes. Deriving
 *     paint bounds from it reads whatever was allocated there this frame:
 *     measured, one unrelated label appearing elsewhere moved a departed row's
 *     reported damage from `{0,65 36x20}` to `{0,73 291x20}`. A stale region,
 *     not a crash — and precisely the class of bug this walk exists to fix.
 *   - `c->final_rect` for children is likewise a plain value.
 *
 * Because `render_rect` is only ever set by activation, this walk stays silent
 * for a node whose last frame was built but never presented — one transient
 * frame's worth of under-report, against damage read out of recycled memory.
 *
 * This is a store scan, not a tree walk — no allocation, and it reads the store
 * array the frame already touches. */
static void damage_walk_leaving(const lens *ui, damage_accum *acc) {
    const lens_store *s = &ui->store;
    for (uint32_t i = s->live_head; i != LENSI_STORE_LINK_NONE; i = s->slots[i].next_live) {
        const lens_node *n = s->slots[i].node;
        if (!n || n->phase != LENS_NODE_LEAVING)
            continue;
        if (n->has_render_rect && n->render_paint_rect.w > 0.0f && n->render_paint_rect.h > 0.0f)
            damage_push(acc, ui, n->render_paint_rect);
        /* Children still hanging off the departed node: a container paints
         * nothing of its own, so the node's own paint misses a removed panel's
         * text. Their `paint_bounds` is captured per node by the same snapshot
         * for exactly this reason. */
        for (const lens_node *c = n->first_child; c; c = c->next_sibling)
            damage_push(acc, ui, c->has_render_rect ? c->render_paint_rect : c->final_rect);
    }
}

/* Damage for one ghost subtree. A ghost node carries its own box, opacity and
 * command list, but no `lens_node` — so its paint extent is resolved command by
 * command through the same predictor the live walk uses, which is what keeps the
 * fading outline/halo fringe from being left behind. */
static void damage_push_ghost(damage_accum *acc, const lens *ui, const lens_ghost_node *g) {
    if (!g)
        return;
    flux_rect hit = (flux_rect){0, 0, 0, 0};
    for (uint32_t i = 0; i < g->cmd_count; i++) {
        const flux_rect paint = lensi_cmd_paint_extent(g->final_rect, &g->cmds[i].cmd);
        hit = rect_union_bounds(hit, paint);
    }
    /* No commands of its own (a departing container): the children paint, and the
     * container's box is the only thing that can still say where. */
    if (hit.w <= 0.0f || hit.h <= 0.0f)
        hit = g->final_rect;
    damage_push(acc, ui, hit);
    for (const lens_ghost_node *c = g->first_child; c; c = c->next_sibling)
        damage_push_ghost(acc, ui, c);
}

static uint32_t damage_resolve(const lens *ui, const lens_damage_opts *opts) {
    flux_rect *out = opts ? opts->rects : nullptr;
    uint32_t capacity = out ? opts->capacity : 0;
    if (capacity > LENS_DAMAGE_RECTS_CAPACITY)
        capacity = LENS_DAMAGE_RECTS_CAPACITY;

    /* Two fixed buffers: raw accumulation and the normalized result. Both are
     * bounded by the public contract, so the walk never allocates. */
    flux_rect raw[LENS_DAMAGE_RECTS_CAPACITY];
    flux_rect normalized[LENS_DAMAGE_RECTS_CAPACITY];
    damage_accum acc = {.rects = raw, .capacity = LENS_DAMAGE_RECTS_CAPACITY};

    damage_walk(ui, ui->root, false, &acc);
    damage_walk_leaving(ui, &acc);

    /* The caret rect is damage only when its visible phase flipped
     * (ADR-0151). Its *existence* is not a pixel change — pushing the rect
     * unconditionally meant a focused field dirtied its caret box every
     * frame, which is the same defect the repaint query carried. */
    if (lensi_caret_phase_changed(ui))
        damage_push(&acc, ui, ui->caret_rect);
    /* The tooltip is painted outside the draw list, below the anchor rather than
     * at it (replay.c: the box is `{anchor.x, anchor.y + anchor.h + 4, ...}`).
     * Reporting the anchor would leave both the appearing box and the erased band
     * of a disappearing one undamaged, so the painted extent is derived from the
     * same measurements replay uses. */
    if (ui->tooltip.active || ui->prev_tooltip_active) {
        /* `painted` was measured at `lensi_tooltip` time, while the context was
         * mutable — a damage walk cannot re-shape text. It survives the reset to
         * `active = false`, so when a tooltip disappears the last painted box is
         * exactly the band that has to be erased. The anchor is reported as well:
         * it is where the widget sits, and a tooltip that moved leaves its previous
         * position behind. */
        damage_push(&acc, ui, ui->tooltip.painted);
        damage_push(&acc, ui, ui->tooltip.anchor);
    }
    /* Fade ghosts paint outside the live tree (ADR-0078). Their damage must be
     * their *paint*, not their container box: a ghost's commands are the same ones
     * replay emits — outlines, icon halos, connected-tab shoulders — and those
     * reach outside the node rectangle. Reporting the box alone would leave the
     * fringe of a fading ghost frozen mid-fade. */
    for (uint32_t i = 0; i < ui->ghost_count; i++) {
        const lens_ghost *g = ui->ghosts[i];
        if (!g || !g->root)
            continue;
        damage_push_ghost(&acc, ui, g->root);
    }

    /* Only the entries actually written may be read: `acc.count` includes any
     * that overflowed. */
    uint32_t written = acc.count < acc.capacity ? acc.count : acc.capacity;
    uint32_t count = damage_normalize(raw, written, normalized);

    /* A frame that needs a repaint but names no region falls back to the whole
     * viewport, as one rectangle, so the failure stays visible in the list instead
     * of silently becoming "no damage". */
    if (count == 0) {
        const flux_rect viewport = {0, 0, ui->input.display_size.x, ui->input.display_size.y};
        const flux_rect fallback =
            viewport.w > 0.0f && viewport.h > 0.0f
                ? viewport
                : (ui->root ? ui->root->final_rect : (flux_rect){0, 0, 0, 0});
        if (fallback.w <= 0.0f || fallback.h <= 0.0f)
            return 0;
        normalized[0] = rect_intersect_bounds(
            fallback, (flux_rect){0, 0, ui->input.display_size.x, ui->input.display_size.y});
        count = normalized[0].w > 0.0f && normalized[0].h > 0.0f ? 1 : 0;
        if (count == 0)
            return 0;
    }

    /* Caller's slice limits how much of the set can be delivered. */
    if (count > capacity) {
        if (capacity == 0)
            return count;
        out[0] = damage_bbox(normalized, count);
        return out[0].w > 0.0f && out[0].h > 0.0f ? 1 : 0;
    }
    if (count)
        memcpy(out, normalized, count * sizeof *out);

    /* An overflowed accumulation is reported as a one-rect answer *in addition*
     * to the regions that fit, because the omitted ones are unknown: any list
     * that excluded them would under-cover, and under-covering leaves stale
     * pixels on screen. The caller sees one full-viewport rect it can repaint.
     * Callers that need the true regions raise the bound, which is a compile-time
     * contract (`LENS_DAMAGE_RECTS_CAPACITY`). */
    if (acc.overflowed) {
        if (capacity == 0)
            return count;
        const flux_rect viewport = {0, 0, ui->input.display_size.x, ui->input.display_size.y};
        const flux_rect cover =
            viewport.w > 0.0f && viewport.h > 0.0f ? viewport : damage_bbox(normalized, count);
        out[0] = cover;
        return cover.w > 0.0f && cover.h > 0.0f ? 1 : 0;
    }
    return count;
}

uint32_t lens_damage_rects(const lens *ui, const lens_damage_opts *opts) {
    if (!ui || !lens_frame_needs_repaint(ui))
        return 0;
    return damage_resolve(ui, opts);
}

void lens_set_reduced_motion(lens *ui, bool reduced) {
    if (ui)
        ui->reduced_motion = reduced;
}
bool lens_reduced_motion(const lens *ui) {
    return ui && ui->reduced_motion;
}

/* ---- style scope stack (ADR-0061 item 4) --------------------------- */

void lens_push_style(lens *ui, lens_style style) {
    if (!ui)
        return;
    if (ui->style_top >= LENSI_STYLE_STACK_MAX) {
        lensi_set_overflow(ui);
        return;
    }
    ui->style_stack[ui->style_top++] = style;
}

void lens_pop_style(lens *ui) {
    if (ui && ui->style_top > 0)
        ui->style_top--;
}

/* ------------------------------------------------------------------ */
/*  Frame lifecycle                                                   */
/* ------------------------------------------------------------------ */

void lens_begin(lens *ui, const lens_input *input) {
    FLUX_PROFILE_FRAME_START("lens.frame");
    FLUX_PROFILE_ZONE(z_begin, "lens.begin");
    /* The UI frame is the ledger's frame-lifetime unit (ADR-0129 decision 4).
     * lens_begin opens it and lens_end closes it, and the balance is enforced
     * at the frame boundary by flux_observe_frame_drain_check, which flux's
     * frame present/abandon paths already call. A lens frame that opens
     * without closing is the exact regression class that per-frame
     * enforcement exists to catch within one frame instead of at exit. */
    if (ui->device)
        FLUX_OBSERVE_ACQUIRE(ui->device, FLUX_SCOPE_CONTEXT, FLUX_LIFETIME_FRAME, "frame",
                             "lens/core", 0, FLUX_SITE_NONE);
    ui->building = true;
    ui->frame++;
    ui->overflow = false;
    ui->duplicate_ids = false;
    ui->anim_pending = false; /* set true by any eased value still in transit */
    flux_arena_reset(&ui->arena);
    lensi_ghost_begin_frame(ui); /* ghosts survive the reset; clear refresh marks */

    /* Size-aware copy (ADR-0036): zero = legacy/trust full struct; >0
     * clamps to min(caller, lib) so apps compiled against older or newer
     * headers degrade cleanly without ABI break. */
    ui->input = (lens_input){0};
    if (input) {
        size_t want = input->size ? input->size : sizeof(lens_input);
        if (want > sizeof(lens_input))
            want = sizeof(lens_input);
        memcpy(&ui->input, input, want);
        ui->input.size = sizeof(lens_input);
        /* A hostile or buggy host key_count must never drive reads past the
         * fixed keys[] array. */
        if (ui->input.key_count > LENS_INPUT_MAX_KEYS)
            ui->input.key_count = LENS_INPUT_MAX_KEYS;
    }
    memset(ui->key_consumed, 0, sizeof ui->key_consumed);
    ui->menu_nav = 0;
    ui->caret_rect = (flux_rect){0, 0, 0, 0};  /* refreshed by text widget */
    ui->text_context = (lens_text_context){0}; /* refreshed alongside it */
    /* Ownership is per-frame, like the rect it describes: whichever producer
     * claims the caret *this* frame decides whether lens owns its blink
     * (ADR-0151). A stale `true` would let a widget-owned caret keep claiming
     * a clock after the host took the caret back. */
    ui->caret_lens_owned = false;
    /* Advance the blink clock here, before any widget runs, so the phase is
     * identical for the skin draw, the repaint query and the damage walk
     * (ADR-0151). Doing it at widget-build time would let a frame that
     * builds two text widgets observe two different phases. */
    lensi_caret_advance_blink(ui);
    if (!ui->input.mouse_down[0] || ui->input.mouse_released[0]) {
        ui->dnd_source.active = false;
    }
    if (ui->frame > ui->dnd_drop.drop_frame + 1) {
        ui->dnd_drop.dropped = false;
        ui->dnd_drop.drop_len = 0;
    }

    /* per-frame build state */
    ui->id_top = 0;
    ui->cont_top = 0;
    ui->have_next_flex = ui->have_next_size = false;
    ui->have_next_col_span = ui->have_next_row_span = ui->have_next_grid_pos = false;
    ui->next_flex = 0;
    ui->next_w = ui->next_h = 0;
    ui->next_col_span = ui->next_row_span = 0;
    ui->next_grid_col = ui->next_grid_row = 0;
    ui->next_disabled = false;
    ui->next_error = false;
    ui->next_placeholder = NULL;
    ui->next_style = lens_style_init();
    ui->style_top = 0;  /* style scopes are frame-scoped (ADR-0061): a
                         * forgotten lens_pop_style cannot leak across frames */
    ui->opacity = 1.0f; /* likewise: a forgotten lens_set_opacity restore
                         * cannot dim the next frame */
    ui->click_hit_focusable = false;
    ui->scroll_hot_id = 0;
    ui->cursor_hint = LENS_CURSOR_DEFAULT;
    ui->tab_order = NULL;
    ui->tab_count = 0; /* tab_cap retained: see the node cmd_cap note */
    ui->last_response = (lens_response){0};
    ui->last_node = NULL;
    /* Band buckets are arena-backed; they reset with the arena and are
     * rebuilt by lensi_place_bucket at lens_end. The per-band caps are
     * retained across the reset for the same reason as cmd_cap: the
     * rebuild allocates at last frame's high-water size in one step
     * instead of re-walking the doubling chain. */
    for (uint32_t b = 0; b < (uint32_t)LENS_BAND_COUNT; b++) {
        ui->bands[b] = NULL;
        ui->band_counts[b] = 0;
    }
    ui->prev_tooltip_active = ui->tooltip.active;
    ui->tooltip.active = false;

#ifdef LENS_DEBUG_BORROWS
    /* Borrow-checker reset (ADR-0085): a new frame starts with an empty
     * registered-borrow set; widgets re-register each label pointer as
     * they build. The check fires only when a *cross-frame* borrow
     * slips through (a pointer from frame N-1 handed to frame N), which
     * is the dangling-stack-string hazard. Same-frame pointers are
     * always registered by the widget that uses them. */
    ui->borrow_count = 0;
#endif

    /* implicit root: a column container covering the display (ADR-0028) */
    lens_id root_id = lensi_hash("##root", 6, 0);
    ui->id_stack[ui->id_top++] = root_id;

    lens_node *root = lensi_store_touch(ui, root_id);
    ui->root = root;
    if (root) {
        root->is_container = true;
        root->axis = LENS_COLUMN;
        root->cross = LENS_STRETCH;
        root->gap = ui->theme.gap;
        root->pad = 0.0f;
        ui->cont_stack[ui->cont_top++] = root;
    }
    FLUX_PROFILE_ZONE_END(z_begin);
}

void lens_end(lens *ui) {
    FLUX_PROFILE_ZONE(z_end, "lens.end");
    /* close any containers the caller left open */
    while (ui->cont_top > 1)
        lensi_open_container_pop(ui);

    /* Close the frame-lifetime row opened by lens_begin (ADR-0129 decision 4).
     * Balanced here; an early return or a throw between begin and end leaves
     * it open, which is precisely what the frame-boundary check reports. */
    if (ui->device)
        FLUX_OBSERVE_RELEASE(ui->device, FLUX_SCOPE_CONTEXT, "frame", "lens/core", 0);

    lensi_store_reap(ui);    /* phase transitions + GC (ADR-0027) */
    lensi_layout_solve(ui);  /* two-pass measure/arrange, ABS-aware (ADR-0028/0060) */
    lensi_place_bucket(ui);  /* bucket ABS nodes into z bands (ADR-0060)  */
    lensi_place_dismiss(ui); /* click-outside + Escape, post-build        */
    lensi_focus_tab(ui);     /* Tab / Shift+Tab focus traversal     */

    /* Click outside any focusable widget clears focus. */
    if (ui->input.mouse_pressed[LENS_MOUSE_LEFT] && !ui->click_hit_focusable) {
        ui->focused_id = 0;
        ui->focus_visible = false;
    }

    /* Scrollbar chrome is emitted here — after the whole tree (base +
     * placed subtrees) is solved and placed, before damage tracking —
     * because layout itself must not author draw commands (ADR-0059). */
    lensi_scrollbars_emit(ui);

    lensi_mark_dirty(ui);      /* compute subtree_changed for culling */
    lensi_ghost_capture(ui);   /* freeze leaving subtrees (ADR-0078) */
    lensi_ghost_end_frame(ui); /* expire unrefreshed ghosts */

    /* An AT activation that no widget consumed this frame (the node
     * vanished, is not focusable, or is disabled) is dropped: requests
     * are single-frame, never queued (ADR-0062). */
    ui->a11y_activate_id = 0;

    /* Modal focus trap is per-frame; reset so a frame with no open modal
     * falls back to whole-range Tab cycling (ADR-0039). The nesting stack
     * resets with it. */
    ui->modal_active = false;
    ui->modal_tab_lo = ui->modal_tab_hi = 0;
    ui->modal_trap_depth = 0;

    /* Advance immutable scene snapshot generation (ADR-0094) */
    ui->generation++;
    ui->building = false;
    FLUX_PROFILE_ZONE_END(z_end);
    FLUX_PROFILE_FRAME_END("lens.frame");
}

void lens_set_focus(lens *ui, lens_id id) {
    if (ui) {
        ui->focused_id = id;
        /* Programmatic focus is not keyboard navigation: no focus ring
         * until the next Tab traversal (ADR-0058). */
        ui->focus_visible = false;
    }
}

void lens_a11y_activate(lens *ui, lens_id id) {
    /* Record only; the next build's lensi_interact consumes it (ADR-0062).
     * A second call before the frame replaces the first — AT-SPI actions
     * are synchronous/sequential, so one pending slot is enough. */
    lens_node *n = ui ? lensi_store_find(ui, id) : nullptr;
    if (n && n->has_prev && n->phase != LENS_NODE_LEAVING)
        ui->a11y_activate_id = id;
}
lens_id lens_active(const lens *ui) {
    return ui ? ui->active_id : 0;
}
bool lens_focused(const lens *ui, lens_id id) {
    return ui && ui->focused_id == id;
}
lens_response lens_get_response(const lens *ui) {
    return ui ? ui->last_response : (lens_response){0};
}
lens_cursor_hint lens_get_cursor_hint(const lens *ui) {
    return ui ? ui->cursor_hint : LENS_CURSOR_DEFAULT;
}
void lens_set_cursor_hint(lens *ui, lens_cursor_hint hint) {
    if (ui)
        ui->cursor_hint = hint;
}

lens_node *lens_root(lens *ui) {
    return ui ? ui->root : NULL;
}
lens_node *lens_find(lens *ui, lens_id id) {
    return ui ? lensi_store_find(ui, id) : NULL;
}

uint64_t lens_generation(const lens *ui) {
    return ui ? ui->generation : 0;
}

uint64_t lens_last_presented_generation(const lens *ui) {
    return ui ? ui->last_presented_generation : 0;
}
