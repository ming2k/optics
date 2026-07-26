/* context.c — lens lifecycle and the per-frame envelope (ADR-0024/0009). */

#include "../internal.h"

const char *lens_version_string(void) {
    return "0.1.0-dev";
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

flux_result lens_create(const lens_desc *desc, lens **out) {
    if (!out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    *out = NULL;

    lens *ui = calloc(1, sizeof *ui);
    if (!ui)
        return FLUX_ERROR_OUT_OF_MEMORY;

    ui->device = desc ? desc->device : NULL;
    if (ui->device) {
        flux_device *d = flux_device_retain(ui->device);
        (void)d;
    }

    ui->theme = (desc && desc->theme.size) ? desc->theme : lens_theme_default();
    lensi_theme_normalize(&ui->theme);
    ui->scale = (desc && desc->scale > 0.0f) ? desc->scale : 1.0f;
    if (desc)
        ui->clipboard = desc->clipboard;

    size_t arena_bytes =
        (desc && desc->arena_bytes) ? desc->arena_bytes : LENSI_DEFAULT_ARENA_BYTES;
    flux_result r = flux_arena_init(&ui->arena, arena_bytes, NULL);
    if (r != FLUX_OK)
        goto fail_arena;

    uint32_t cap = (desc && desc->store_capacity) ? desc->store_capacity : LENSI_DEFAULT_STORE_CAP;
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
    if (t->active_indicator_width < 0.0f)
        t->active_indicator_width = 0.0f;
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

void lens_set_theme(lens *ui, lens_theme theme) {
    if (!ui)
        return;
    if (!theme.size)
        theme = lens_theme_default();
    ui->theme = theme;
    lensi_theme_normalize(&ui->theme);
}

lens_theme lens_get_theme(const lens *ui) {
    return ui ? ui->theme : lens_theme_default();
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

    /* Floating layers (popups, menus, panels) are independent sub-roots:
     * their damage is not part of the base tree, and an open/close or a
     * z-order change alters the layer set itself. prev_overlay_layer_ids
     * holds last frame's set (carried across the arena reset). */
    if (ui->overlay_layer_count != ui->prev_overlay_layer_count)
        return true;
    for (uint32_t i = 0; i < ui->overlay_layer_count; i++) {
        lens_node *n = ui->overlay_layers[i];
        if (!n || n->subtree_changed)
            return true;
        if (n->id != ui->prev_overlay_layer_ids[i])
            return true;
    }

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

/* ------------------------------------------------------------------ */
/*  Frame lifecycle                                                   */
/* ------------------------------------------------------------------ */

void lens_begin(lens *ui, const lens_input *input) {
    /* Carry last frame's floating-layer ids across the arena reset so
     * this frame's eclipse checks use the layers that are actually on
     * screen (their prev_rect geometry), independent of build order. */
    ui->prev_overlay_layer_count = 0;
    for (uint32_t i = 0; i < ui->overlay_layer_count && i < LENSI_OVERLAY_MAX; ++i) {
        if (ui->overlay_layers[i])
            ui->prev_overlay_layer_ids[ui->prev_overlay_layer_count++] = ui->overlay_layers[i]->id;
    }
    flux_arena_reset(&ui->arena);
    ui->frame++;
    ui->overflow = false;
    ui->anim_pending = false; /* set true by any eased value still in transit */

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
    }
    ui->caret_rect = (flux_rect){0, 0, 0, 0}; /* refreshed by text widget */

    /* per-frame build state */
    ui->id_top = 0;
    ui->cont_top = 0;
    ui->have_next_flex = ui->have_next_size = false;
    ui->next_flex = 0;
    ui->next_w = ui->next_h = 0;
    ui->next_disabled = false;
    ui->next_error = false;
    ui->next_placeholder = NULL;
    ui->click_hit_focusable = false;
    ui->hot_id = 0;
    ui->scroll_hot_id = 0;
    ui->cursor_hint = LENS_CURSOR_DEFAULT;
    ui->tab_order = NULL;
    ui->tab_count = ui->tab_cap = 0;
    ui->last_response = (lens_response){0};
    ui->last_node = NULL;
    ui->overlay_layers = NULL; /* arena-backed; resets with the arena */
    ui->overlay_layer_count = ui->overlay_layer_cap = 0;
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

    lensi_store_reap(ui);      /* phase transitions + GC (ADR-0027) */
    lensi_layout_solve(ui);    /* two-pass measure/arrange (ADR-0028) */
    lensi_overlay_layout(ui);  /* place open floating layers (ADR-0037) */
    lensi_overlay_dismiss(ui); /* click-outside + Escape, post-build   */
    lensi_focus_tab(ui);       /* Tab / Shift+Tab focus traversal     */

    /* Click outside any focusable widget clears focus. */
    if (ui->input.mouse_pressed[LENS_MOUSE_LEFT] && !ui->click_hit_focusable)
        ui->focused_id = 0;

    lensi_mark_dirty(ui);         /* compute subtree_changed for culling */
    lensi_overlay_mark_dirty(ui); /* same for floating-layer sub-roots   */

    /* Modal focus trap is per-frame; reset so a frame with no open modal
     * falls back to whole-range Tab cycling (ADR-0039). */
    ui->modal_active = false;
    ui->modal_tab_lo = ui->modal_tab_hi = 0;
}

void lens_set_focus(lens *ui, lens_id id) {
    if (ui)
        ui->focused_id = id;
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
