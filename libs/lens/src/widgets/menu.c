/* menu.c — menu bar, context menu, submenu, and menu items (ADR-0017).
 *
 * Built on the ADR-0014 overlay layer. A menu is an overlay laid out as a
 * vertical list of items; a menu bar is a horizontal row of triggers with
 * click-then-drag switch behaviour; a context menu anchors at the cursor;
 * a submenu is a nested overlay anchored to its parent item.
 *
 * State that must survive across frames lives in the retained store via
 * lens_node_state: bar-switch tracking, submenu hover-dwell timers, and
 * the cursor position captured at context-menu open time. */

#include "../internal.h"
#include <stdio.h>

/* Per-node retained state sizes. */
typedef struct {
    bool open;
    float hover_t;     /* smoothed hover (for submenu items) */
    float dwell;       /* accumulated hover dwell toward opening a submenu */
    bool submenu_open; /* cached open state for the submenu overlay */
} lens_menu_item_state;

typedef struct {
    bool any_open;   /* a menu of this bar is open (enables switch-on-hover) */
    char active[64]; /* label of the currently-open bar menu, "" = none */
} lens_menubar_state;

typedef struct {
    flux_point at; /* cursor position captured at open time */
} lens_ctxmenu_state;

/* ------------------------------------------------------------------ */
/*  Overlay option preset shared by all menus                          */
/* ------------------------------------------------------------------ */

static lens_overlay_opts menu_panel_opts(lens *ui) {
    const lens_theme *t = &ui->theme;
    return (lens_overlay_opts){
        .pad = t->padding * 0.5f,
        .gap = 1.0f,
        .bg = t->color_hover,
        .border = t->color_border,
        .border_width = t->border_width,
        .radius = t->corner_radius,
        .cross = LENS_STRETCH,
    };
}

/* ------------------------------------------------------------------ */
/*  Menu item                                                          */
/* ------------------------------------------------------------------ */

/* A full-width row: optional check/radio glyph + label (left), shortcut
 * (right, dimmed). Non-interactive + dimmed when disabled. On click it
 * fires and the whole open menu stack is dismissed. */
bool lens_menu_item(lens *ui, const char *label, const char *shortcut) {
    return lens_menu_item_flags(ui, label, shortcut, 0);
}

bool lens_menu_item_disabled(lens *ui, const char *label, const char *shortcut) {
    return lens_menu_item_flags(ui, label, shortcut, LENS_MENU_DISABLED);
}

bool lens_menu_item_flags(lens *ui, const char *label, const char *shortcut, uint32_t flags) {
    const lens_theme *t = &ui->theme;
    bool disabled = (flags & LENS_MENU_DISABLED) || ui->next_disabled;
    bool checked = (flags & LENS_MENU_CHECKED) != 0;
    bool radio = (flags & LENS_MENU_RADIO) != 0;
    ui->next_disabled = false;
    ui->next_error = false;

    lens_id id = lensi_gen_widget_id(ui, label);
    lens_node *n = lensi_store_touch(ui, id);
    if (!n)
        return false;
    lensi_link_child(ui, n);
    n->is_container = false;

    /* Measure: label + glyph (left), shortcut (right). */
    lens_text_metrics tm = lensi_text_measure_label(ui, label, t->font_size, 0.0f);
    float glyph = (checked || radio) ? (t->font_size * 0.8f + t->padding) : 0.0f;
    float sc_w = 0.0f;
    if (shortcut)
        sc_w = lensi_text_measure_label(ui, shortcut, t->font_size * 0.9f, 0.0f).width +
               t->padding * 2.0f;
    float w = glyph + tm.width + sc_w + 2.0f * t->padding;
    float h = n->fixed_h > 0 ? n->fixed_h : (t->font_size + 2.0f * t->padding);
    if (n->fixed_w > 0)
        w = n->fixed_w;
    n->measured = (flux_point){w, h};

    lens_response r = lensi_interact(ui, n, false, disabled);
    lens_menu_item_state *st = lens_node_state(n, sizeof *st);

    uint32_t sem_flags = (disabled ? LENS_A11Y_DISABLED : 0) | (checked ? LENS_A11Y_CHECKED : 0);
    lensi_node_semantics(ui, n, LENS_ROLE_BUTTON, label, shortcut, sem_flags);

    float dt = ui->input.dt_seconds;
    if (!disabled)
        n->hover_t = lensi_approach(ui, n->hover_t, r.hovered ? 1.f : 0.f, dt, 15.f);

    /* background fill on hover */
    if (n->hover_t > 0.01f && !disabled) {
        flux_color bg = lensi_lerp_color(t->color_hover, t->color_active, n->hover_t);
        lensi_drawlist_push(
            ui, n,
            (lens_draw_cmd){
                .kind = LENS_DRAW_RECT, .rel = {0, 0, 0, 0}, .color = bg, .radius = 0.0f});
    }

    /* check / radio glyph on the left */
    if (checked) {
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                            .rel = {t->padding, (h - tm.height) * 0.5f, 0, 0},
                                            .color = t->color_accent,
                                            .text = radio ? "●" : "✓",
                                            .text_size = t->font_size * 0.8f});
    }

    /* label */
    float label_x = t->padding + glyph;
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                        .rel = {label_x, (h - tm.height) * 0.5f, 0, 0},
                                        .color = disabled ? t->color_disabled : t->color_fg,
                                        .text = label,
                                        .text_size = t->font_size});

    /* shortcut right-aligned */
    if (shortcut && sc_w > 0.0f) {
        lensi_drawlist_push(ui, n,
                            (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                            .rel = {w - t->padding - sc_w + t->padding,
                                                    (h - tm.height) * 0.5f, 0, 0},
                                            .color = t->color_disabled,
                                            .text = shortcut,
                                            .text_size = t->font_size * 0.9f});
    }

    ui->last_response = r;
    if (r.clicked && !disabled) {
        lens_menubar_close_all_open(ui);
        return true;
    }
    return false;
}

/* A thin separator line, one row tall, non-interactive. */
void lens_menu_separator(lens *ui) {
    const lens_theme *t = &ui->theme;
    lens_id id = lensi_gen_container_id(ui, "sep");
    lens_node *n = lensi_store_touch(ui, id);
    if (!n)
        return;
    lensi_link_child(ui, n);
    n->is_container = false;
    n->measured = (flux_point){0, t->padding};
    lensi_drawlist_push(
        ui, n,
        (lens_draw_cmd){.kind = LENS_DRAW_RECT,
                        .rel = {t->padding, t->padding * 0.5f, -t->padding * 2.0f, 1.0f},
                        .color = t->color_border,
                        .radius = 0.0f});
    ui->last_response = (lens_response){.id = id};
}

/* ------------------------------------------------------------------ */
/*  Menu bar                                                           */
/* ------------------------------------------------------------------ */

/* Close every overlay opened by a menu bar (the bar menu + any submenus
 * stacked above it). We close the whole stack so firing an item dismisses
 * the cascade in one frame. */
void lens_menubar_close_all_open(lens *ui) {
    if (!ui)
        return;
    ui->open_overlay_count = 0;
}

bool lens_menubar_begin(lens *ui, const char *id) {
    lens_row(ui);
    /* Record bar state on a retained node keyed by the bar id. */
    (void)lensi_gen_container_id(ui, id);
    return true;
}

void lens_menubar_end(lens *ui) {
    lens_close(ui);
}

/* Derive the overlay-popup label ("label##menu") so the popup node does
 * not share the trigger's id (which would corrupt prev_rect — same lesson
 * as dropdown.c's "##ov"). Returns the lens_id under the current scope. */
static lens_id menu_overlay_id(lens *ui, const char *label, char *buf, size_t cap) {
    int nw = snprintf(buf, cap, "%s##menu", label);
    if (nw < 0 || (size_t)nw >= cap) {
        size_t l = strlen(label);
        if (l > cap - 8)
            l = cap - 8;
        memcpy(buf, label, l);
        memcpy(buf + l, "##menu", 7);
    }
    return lensi_gen_widget_id(ui, buf);
}

/* A menu trigger inside a bar: opens its overlay on click, and switches
 * when hovered while another menu of the same bar is open. Returns true
 * when the menu body should build (overlay open). */
bool lens_menu_begin(lens *ui, const char *label) {
    const lens_theme *t = &ui->theme;
    lens_id trig_id = lensi_gen_widget_id(ui, label);
    lens_node *n = lensi_store_touch(ui, trig_id);
    if (!n)
        return false;
    lensi_link_child(ui, n);
    n->is_container = false;

    lens_text_metrics tm = lensi_text_measure_label(ui, label, t->font_size, 0.0f);
    float w = tm.width + 2.0f * t->padding;
    float h = t->font_size + 2.0f * t->padding;
    n->measured = (flux_point){w, h};

    lens_response r = lensi_interact(ui, n, true, false);
    lensi_node_semantics(ui, n, LENS_ROLE_BUTTON, label, NULL, 0);

    /* The open state lives on the *overlay* id, not the trigger id, so the
     * two nodes stay distinct (ADR-0017; see dropdown.c "##ov"). */
    char ov_label[80];
    lens_id ov_id = menu_overlay_id(ui, label, ov_label, sizeof ov_label);
    bool open = lensi_overlay_is_open_id(ui, ov_id);

    /* click opens; hover switches when a sibling menu is already open */
    bool any_bar_open = (ui->open_overlay_count > 0);
    if (r.clicked) {
        lensi_overlay_open_id_pub(ui, ov_id, true);
        open = true;
    } else if (r.hovered && any_bar_open && !open) {
        /* bar-switch: close the current sibling and open this one */
        lens_menubar_close_all_open(ui);
        lensi_overlay_open_id_pub(ui, ov_id, true);
        open = true;
    }

    float dt = ui->input.dt_seconds;
    n->hover_t = lensi_approach(ui, n->hover_t, (open || r.hovered) ? 1.f : 0.f, dt, 15.f);

    flux_color bg =
        open ? t->color_hover : lensi_lerp_color(t->color_bg, t->color_hover, n->hover_t);
    lensi_drawlist_push(
        ui, n,
        (lens_draw_cmd){
            .kind = LENS_DRAW_RECT, .rel = {0, 0, 0, 0}, .color = bg, .radius = t->corner_radius});
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                        .rel = {t->padding, (h - tm.height) * 0.5f, 0, 0},
                                        .color = t->color_fg,
                                        .text = label,
                                        .text_size = t->font_size});

    ui->last_response = r;

    if (!open)
        return false;

    /* open the overlay below the trigger */
    flux_rect anchor = n->has_prev ? n->prev_rect : (flux_rect){0, 0, w, h};
    return lens_overlay_begin(ui, ov_label, anchor, menu_panel_opts(ui));
}

void lens_menu_end(lens *ui) {
    lens_overlay_end(ui);
}

/* ------------------------------------------------------------------ */
/*  Submenu                                                            */
/* ------------------------------------------------------------------ */

/* Anchors to the right of the parent item (the last node linked). */
bool lens_submenu_begin(lens *ui, const char *label) {
    const lens_theme *t = &ui->theme;
    lens_node *parent = ui->last_node; /* the item row this submenu hangs off */
    lens_id id = lensi_gen_widget_id(ui, label);
    lens_node *n = lensi_store_touch(ui, id);
    if (!n)
        return false;
    lensi_link_child(ui, n);
    n->is_container = false;

    lens_text_metrics tm = lensi_text_measure_label(ui, label, t->font_size, 0.0f);
    float w = tm.width + 2.0f * t->padding;
    float h = t->font_size + 2.0f * t->padding;
    n->measured = (flux_point){w, h};

    lens_response r = lensi_interact(ui, n, false, false);
    lens_menu_item_state *st = lens_node_state(n, sizeof *st);

    /* Dwell: accumulate hover time; open after ~0.25s. Reset on leave. */
    float dt = ui->input.dt_seconds;
    if (r.hovered)
        st->dwell += dt;
    else
        st->dwell = 0.0f;
    bool open = lensi_overlay_is_open_id(ui, id) || st->dwell > 0.25f;
    if (open && !lensi_overlay_is_open_id(ui, id))
        lensi_overlay_open_id_pub(ui, id, true);

    n->hover_t = lensi_approach(ui, n->hover_t, r.hovered ? 1.f : 0.f, dt, 15.f);
    flux_color bg = lensi_lerp_color(t->color_hover, t->color_active, n->hover_t);
    lensi_drawlist_push(
        ui, n,
        (lens_draw_cmd){.kind = LENS_DRAW_RECT, .rel = {0, 0, 0, 0}, .color = bg, .radius = 0.0f});
    lensi_drawlist_push(ui, n,
                        (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                        .rel = {t->padding, (h - tm.height) * 0.5f, 0, 0},
                                        .color = t->color_fg,
                                        .text = label,
                                        .text_size = t->font_size});
    /* trailing chevron ▸ */
    lensi_drawlist_push(
        ui, n,
        (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                        .rel = {w - t->padding - tm.height, (h - tm.height) * 0.5f, 0, 0},
                        .color = t->color_disabled,
                        .text = "▸",
                        .text_size = t->font_size});

    ui->last_response = r;
    if (!open)
        return false;

    /* anchor to the right of the parent item's last-frame rect */
    flux_rect anchor = {0, 0, 1, h};
    if (parent && parent->has_prev)
        anchor = (flux_rect){parent->prev_rect.x + parent->prev_rect.w, parent->prev_rect.y, 1,
                             parent->prev_rect.h};
    return lens_overlay_begin(ui, label, anchor, menu_panel_opts(ui));
}

void lens_submenu_end(lens *ui) {
    lens_overlay_end(ui);
}

/* ------------------------------------------------------------------ */
/*  Context menu                                                       */
/* ------------------------------------------------------------------ */

void lens_context_menu_open(lens *ui, const char *id, flux_rect owner_rect) {
    if (!ui || !id)
        return;
    lens_id lid = lensi_gen_widget_id(ui, id);
    /* stash the cursor position for anchoring */
    lens_node *n = lensi_store_touch(ui, lid);
    if (n) {
        lens_ctxmenu_state *st = lens_node_state(n, sizeof *st);
        st->at = ui->input.cursor;
    }
    lensi_overlay_open_id_pub(ui, lid, true);
    (void)owner_rect;
}

bool lens_context_menu_begin(lens *ui, const char *id) {
    if (!ui || !id)
        return false;
    lens_id lid = lensi_gen_widget_id(ui, id);
    if (!lensi_overlay_is_open_id(ui, lid))
        return false;
    lens_node *n = lensi_store_find(ui, lid);
    flux_point at = {0, 0};
    if (n) {
        lens_ctxmenu_state *st = lens_node_state(n, sizeof *st);
        at = st->at;
    }
    flux_rect anchor = {at.x, at.y, 1, 1};
    if (lens_overlay_begin(ui, id, anchor, menu_panel_opts(ui))) {
        if (ui->last_node)
            lensi_node_semantics(ui, ui->last_node, LENS_ROLE_MENU, id, NULL, 0);
        return true;
    }
    return false;
}

void lens_context_menu_end(lens *ui) {
    lens_overlay_end(ui);
}
