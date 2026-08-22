/* context.c — lens lifecycle and the per-frame envelope (ADR-0024/0009). */

#include "../internal.h"

const char *lens_version_string(void) {
    return "0.0.24";
}

void *lensi_alloc(lens *ui, size_t bytes) {
    return ui->device ? flux_device_alloc(ui->device, bytes) : malloc(bytes);
}

void lensi_free(lens *ui, void *ptr) {
    if (!ptr)
        return;
    if (ui->device)
        flux_device_free(ui->device, ptr);
    else
        free(ptr);
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

    lens *ui = calloc(1, sizeof *ui);
    if (!ui)
        return FLUX_ERROR_OUT_OF_MEMORY;

    /* Read the caller's descriptor through the size guard: a caller built
     * against a different lens_desc layout never makes the library read
     * past its own struct. All field reads below come from this local
     * copy, never from `desc` directly. */
    const lens_desc d = lensi_desc_copy_in(desc);
    desc = &d;

    ui->device = d.device;
    if (ui->device) {
        flux_device *dev = flux_device_retain(ui->device);
        (void)dev;
    }

    ui->theme = d.theme.size ? lensi_theme_copy_in(&d.theme) : lens_theme_default();
    lensi_theme_normalize(&ui->theme);
    ui->scale = (d.scale > 0.0f) ? d.scale : 1.0f;
    ui->opacity = 1.0f;
    ui->tooltip.opacity = 1.0f;
    ui->clipboard = d.clipboard;

    size_t arena_bytes = d.arena_bytes ? d.arena_bytes : LENSI_DEFAULT_ARENA_BYTES;
    flux_result r = flux_arena_init(&ui->arena, arena_bytes, NULL);
    if (r != FLUX_OK)
        goto fail_arena;

    uint32_t cap = d.store_capacity ? d.store_capacity : LENSI_DEFAULT_STORE_CAP;
    r = lensi_store_init(ui, cap);
    if (r != FLUX_OK)
        goto fail_store;

    /* Shared text engine (flux-text). Always usable: it degrades to
     * monospace metrics internally when no shaping backend or font is
     * available, so widgets never branch on backend presence. Only fails
     * on allocation, which is non-fatal here (text simply stays blank). */
    if (flux_text_create(&(flux_text_desc){.device = ui->device, .scale = ui->scale}, &ui->text) !=
        FLUX_OK)
        ui->text = NULL;

    *out = ui;
    return FLUX_OK;

fail_store:
    flux_arena_destroy(&ui->arena);
fail_arena:
    if (ui->device)
        flux_device_release(ui->device);
    free(ui);
    return r;
}

void lens_destroy(lens *ui) {
    if (!ui)
        return;
    flux_text_destroy(ui->text); /* null-safe */
    lensi_store_destroy(ui);
    flux_arena_destroy(&ui->arena);
    if (ui->device)
        flux_device_release(ui->device);
    free(ui);
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
    if (!t->color_slider_track)
        t->color_slider_track = t->color_border;
    if (!t->color_slider_fill)
        t->color_slider_fill = t->color_accent;
    if (!t->color_slider_knob)
        t->color_slider_knob = t->color_fg;
    if (t->slider_track_thickness <= 0.0f)
        t->slider_track_thickness = 6.0f;
    if (t->slider_knob_size <= 0.0f)
        t->slider_knob_size = 14.0f;
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
    if (opacity < 0.0f)
        opacity = 0.0f;
    if (opacity > 1.0f)
        opacity = 1.0f;
    ui->opacity = opacity;
}

float lens_opacity(const lens *ui) {
    return ui ? ui->opacity : 1.0f;
}

void lens_set_scale(lens *ui, float scale) {
    if (ui && scale > 0.0f) {
        ui->scale = scale;
        flux_text_set_scale(ui->text, scale); /* null-safe */
    }
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

/* Read-only repaint query for damage-driven hosts (valid between lens_end
 * and the next lens_begin). True when anything the last lens_render would
 * have produced differs from what is already on screen. */
bool lens_frame_needs_repaint(const lens *ui) {
    if (!ui)
        return false;

    /* Base tree damage: lifecycle, geometry, hover/active eases and
     * draw-list hash changes, rolled up bottom-up by lensi_mark_dirty. */
    if (ui->root && ui->root->subtree_changed)
        return true;

    /* Placed subtrees (popups, menus, chrome, backdrops) carry their own
     * subtree_changed (their damage deliberately does not roll up into the
     * root's display-list record — replay.c), so the query consults the
     * band buckets directly. An open/close alters the parent's child
     * sequence, which the base-tree check above already catches. */
    for (uint32_t b = 0; b < (uint32_t)LENS_BAND_COUNT; b++)
        for (uint32_t i = 0; i < ui->band_counts[b]; i++)
            if (ui->bands[b][i] && ui->bands[b][i]->subtree_changed)
                return true;

    /* The tooltip is painted straight from lens_render (no draw list, no
     * node), so only its presence is observable here: active, or active
     * last frame and now gone (its pixels must be erased). */
    if (ui->tooltip.active || ui->prev_tooltip_active)
        return true;

    /* Time-driven state: an eased value (hover/active fade, slide) still
     * in transit — the next frame differs from this one even with no
     * input. */
    if (ui->anim_pending)
        return true;

    /* A focused text widget keeps the caret clock alive: the host paces
     * low-frequency frames for the blink, and each of those frames must
     * actually paint. */
    if (ui->caret_rect.w > 0.0f)
        return true;

    return false;
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
    ui->frame++;
    ui->overflow = false;
    ui->duplicate_ids = false;
    ui->anim_pending = false; /* set true by any eased value still in transit */
    /* Carry last frame's band buckets across the arena reset as per-band
     * prev id lists, so this frame's occlusion checks use the placed nodes
     * that are actually on screen (their prev_rect geometry), independent
     * of build order (ADR-0060). Runs after the per-frame flag reset so a
     * truncation it flags stays flagged. */
    lensi_place_snapshot_prev(ui);
    flux_arena_reset(&ui->arena);

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

    /* per-frame build state */
    ui->id_top = 0;
    ui->cont_top = 0;
    ui->have_next_flex = ui->have_next_size = false;
    ui->next_flex = 0;
    ui->next_w = ui->next_h = 0;
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
}

void lens_end(lens *ui) {
    /* close any containers the caller left open */
    while (ui->cont_top > 1)
        lensi_open_container_pop(ui);

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

    lensi_mark_dirty(ui); /* compute subtree_changed for culling */

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
    if (ui)
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

lens_node *lens_root(lens *ui) {
    return ui ? ui->root : NULL;
}
lens_node *lens_find(lens *ui, lens_id id) {
    return ui ? lensi_store_find(ui, id) : NULL;
}
