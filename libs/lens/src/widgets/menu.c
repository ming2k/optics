/* menu.c — menu bar, context menu, submenu, and menu items (ADR-0040).
 *
 * Built on ADR-0060 placement. A menu is a transient POPUP-band node laid
 * out as a vertical list of items; a menu bar is a horizontal row of
 * triggers with click-then-drag switch behaviour; a context menu anchors
 * at the cursor; a submenu is a nested popup anchored to its parent item.
 *
 * State that must survive across frames lives in the retained store via
 * lens_node_state: bar-switch tracking, submenu hover-dwell timers, and
 * the cursor position captured at context-menu open time. */

#include "../internal.h"
#include <stdio.h>

/* Per-node retained state sizes. */
typedef struct {
    float dwell; /* accumulated hover dwell toward opening a submenu */
} lens_menu_item_state;

typedef struct {
    flux_point at; /* cursor position captured at open time */
} lens_ctxmenu_state;

/* ------------------------------------------------------------------ */
/*  Place option preset shared by all menus                            */
/* ------------------------------------------------------------------ */

static lens_place_opts menu_panel_opts(const lens_style_resolved *rs, flux_rect anchor) {
    return (lens_place_opts){
        .band = LENS_BAND_POPUP,
        .mode = LENS_PLACE_ANCHORED,
        .rect = anchor,
        .transient = true,
        .layout =
            {
                .pad = rs->padding * 0.5f,
                .gap = 1.0f,
                .bg = rs->bg | 0xff000000u,
                .border = rs->border,
                .border_width = rs->border_width,
                .radius = rs->corner_radius,
                .cross = LENS_STRETCH,
            },
    };
}

/* ------------------------------------------------------------------ */
/*  Menu item                                                          */
/* ------------------------------------------------------------------ */

/* Walk to the next/previous semantic sibling row (arrow navigation skips
 * the separator, which carries no semantics). */
static lens_node *menu_sibling(lens_node *n, bool forward) {
    if (forward) {
        for (lens_node *c = n->next_sibling; c; c = c->next_sibling)
            if (c->semantics.role != LENS_ROLE_NONE)
                return c;
        return NULL;
    }
    lens_node *prev = NULL;
    for (lens_node *c = n->parent ? n->parent->first_child : NULL; c && c != n;
         c = c->next_sibling)
        if (c->semantics.role != LENS_ROLE_NONE)
            prev = c;
    return prev;
}

/* Shared focus-move request for menu rows (items and submenu rows): a
 * focused row seeing Up/Down records the direction and consumes the key
 * (so no other widget — an open dropdown's list nav — acts on it this
 * frame). The move itself is applied by menu_apply_nav once the popup's
 * rows are all built. */
static void menu_arrow_nav(lens *ui, lens_node *n) {
    (void)n;
    for (uint32_t i = 0; i < ui->input.key_count; i++) {
        const lens_key_event *k = &ui->input.keys[i];
        if (!k->pressed || ui->key_consumed[i])
            continue;
        if (k->key == LENS_KEY_DOWN)
            ui->menu_nav = 1;
        else if (k->key == LENS_KEY_UP)
            ui->menu_nav = -1;
        else
            continue;
        ui->key_consumed[i] = 1;
    }
}

/* Apply a queued arrow-nav move: walk the focused row's siblings (all
 * built by the time a menu end runs), skipping non-semantic rows (the
 * separator), and move keyboard focus there. */
static void menu_apply_nav(lens *ui) {
    if (!ui->menu_nav || !ui->focused_id)
        return;
    lens_node *n = lensi_store_find(ui, ui->focused_id);
    if (!n)
        return;
    bool forward = ui->menu_nav > 0;
    lens_node *sib = menu_sibling(n, forward);
    if (sib) {
        ui->focused_id = sib->id;
        ui->focus_visible = true; /* arrow nav is keyboard modality (ADR-0058) */
    }
    ui->menu_nav = 0;
}

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
    lens_style eff = lensi_style_effective(ui);
    float font_size = lensi_style_font_size(&eff, t);
    float padding = lensi_style_padding(&eff, t);
    lens_style_resolved rs = lensi_style_resolve(&eff, t,
        disabled ? LENS_STATE_DISABLED : 0);

    lens_id id = lensi_gen_widget_id(ui, label);
    lens_node *n = lensi_store_touch(ui, id);
    if (!n)
        return false;
    lensi_link_child(ui, n);
    n->is_container = false;

    /* Measure: label + glyph (left), shortcut (right). */
    lens_text_metrics tm = lensi_text_measure_label(ui, label, font_size, 0.0f);
    float glyph = (checked || radio) ? (font_size * 0.8f + padding) : 0.0f;
    lens_text_metrics shortcut_tm =
        shortcut ? lensi_text_measure_label(ui, shortcut, font_size * 0.9f, 0.0f)
                 : (lens_text_metrics){0};
    float sc_w = shortcut ? shortcut_tm.width + padding * 2.0f : 0.0f;
    float w = glyph + tm.width + sc_w + 2.0f * padding;
    float h = n->fixed_h > 0 ? n->fixed_h : (font_size + 2.0f * padding);
    if (n->fixed_w > 0)
        w = n->fixed_w;
    n->measured = (flux_point){w, h};

    lens_response r = lensi_interact(ui, n, true, disabled);
    if (r.hovered)
        ui->cursor_hint = LENS_CURSOR_POINTER;
    if (r.focused && !disabled)
        menu_arrow_nav(ui, n);
    (void)lens_node_state(n, sizeof(lens_menu_item_state)); /* reserve for future state */

    uint32_t sem_flags = (disabled ? LENS_A11Y_DISABLED : 0) | (checked ? LENS_A11Y_CHECKED : 0);
    lensi_node_semantics(ui, n, LENS_ROLE_MENUITEM, label, shortcut, sem_flags);

    float dt = ui->input.dt_seconds;
    if (!disabled)
        n->hover_t = lensi_approach(ui, n->hover_t, r.hovered ? 1.f : 0.f, dt, 15.f);

    /* emit — through the replaceable skin (ADR-0059) */
    lensi_skin_emit(ui, n,
                    &(lens_widget_record){
                        .kind = LENS_WIDGET_MENU_ITEM,
                        .state = r.state,
                        .bounds = {0, 0, w, h},
                        .last_bounds = n->prev_rect,
                        .style = rs,
                        .style_fields = eff.fields,
                        .hover_t = n->hover_t,
                        .active_t = n->active_t,
                        .content = {.label = label,
                                    .text = tm,
                                    .shortcut = shortcut,
                                    .shortcut_text = shortcut_tm,
                                    .menu_check = checked,
                                    .menu_radio = radio},
                    });

    ui->last_response = r;
    if (r.clicked && !disabled) {
        lens_menubar_close_all_open(ui);
        return true;
    }
    return false;
}

/* A thin separator line, one row tall, non-interactive. */
void lens_menu_separator(lens *ui) {
    lens_style eff = lensi_style_effective(ui);
    lens_style_resolved rs = lensi_style_resolve(&eff, &ui->theme, 0);
    lens_id id = lensi_gen_container_id(ui, "sep");
    lens_node *n = lensi_store_touch(ui, id);
    if (!n)
        return;
    lensi_link_child(ui, n);
    n->is_container = false;
    n->measured = (flux_point){0, rs.padding};
    lensi_skin_emit(ui, n,
                    &(lens_widget_record){
                        .kind = LENS_WIDGET_MENU_ITEM,
                        .state = 0,
                        .bounds = {0, 0, n->measured.x, n->measured.y},
                        .last_bounds = n->prev_rect,
                        .style = rs,
                        .style_fields = eff.fields,
                        .content = {.menu_separator = true},
                    });
    ui->last_response = (lens_response){.id = id};
}

/* ------------------------------------------------------------------ */
/*  Menu bar                                                           */
/* ------------------------------------------------------------------ */

/* Close every transient opened by a menu bar (the bar menu + any submenus
 * stacked above it). We close the whole stack so firing an item dismisses
 * the cascade in one frame. Pinned transients (a modal, ADR-0039) are NOT
 * menu-owned and are left alone — the menu must not take a pinned dialog
 * down with it. */
void lens_menubar_close_all_open(lens *ui) {
    if (!ui)
        return;
    uint32_t kept = 0;
    for (uint32_t i = 0; i < ui->open_transient_count; i++) {
        if (!ui->open_transients[i].dismissable) {
            ui->open_transients[kept++] = ui->open_transients[i];
        }
    }
    ui->open_transient_count = kept;
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

/* Derive the popup label ("label##menu") so the popup node does
 * not share the trigger's id (which would corrupt prev_rect — same lesson
 * as dropdown.c's "##ov"). Returns the lens_id under the current scope. */
static lens_id menu_popup_id(lens *ui, const char *label, char *buf, size_t cap) {
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

/* A menu trigger inside a bar: opens its popup on click, and switches
 * when hovered while another menu of the same bar is open. Returns true
 * when the menu body should build (popup open). */
bool lens_menu_begin(lens *ui, const char *label) {
    const lens_theme *t = &ui->theme;
    lens_style eff = lensi_style_effective(ui);
    float font_size = lensi_style_font_size(&eff, t);
    float padding = lensi_style_padding(&eff, t);
    lens_style_resolved rs = lensi_style_resolve(&eff, t, 0);
    lens_id trig_id = lensi_gen_widget_id(ui, label);
    lens_node *n = lensi_store_touch(ui, trig_id);
    if (!n)
        return false;
    lensi_link_child(ui, n);
    n->is_container = false;

    lens_text_metrics tm = lensi_text_measure_label(ui, label, font_size, 0.0f);
    float w = tm.width + 2.0f * padding;
    float h = font_size + 2.0f * padding;
    n->measured = (flux_point){w, h};

    lens_response r = lensi_interact(ui, n, true, false);
    lensi_node_semantics(ui, n, LENS_ROLE_BUTTON, label, NULL, 0);

    /* The open state lives on the *popup* id, not the trigger id, so the
     * two nodes stay distinct (ADR-0040; see dropdown.c "##ov"). */
    char ov_label[80];
    lens_id ov_id = menu_popup_id(ui, label, ov_label, sizeof ov_label);
    bool open = lensi_place_is_open_id(ui, ov_id);

    /* click opens; hover switches when a sibling menu is already open */
    bool any_bar_open = (ui->open_transient_count > 0);
    if (r.clicked) {
        lensi_place_open_id_pub(ui, ov_id, true);
        open = true;
    } else if (r.hovered && any_bar_open && !open) {
        /* bar-switch: close the current sibling and open this one */
        lens_menubar_close_all_open(ui);
        lensi_place_open_id_pub(ui, ov_id, true);
        open = true;
    }

    float dt = ui->input.dt_seconds;
    n->hover_t = lensi_approach(ui, n->hover_t, (open || r.hovered) ? 1.f : 0.f, dt, 15.f);

    /* emit — through the replaceable skin (ADR-0059) */
    lensi_skin_emit(ui, n,
                    &(lens_widget_record){
                        .kind = LENS_WIDGET_MENU_ITEM,
                        .state = r.state,
                        .bounds = {0, 0, w, h},
                        .last_bounds = n->prev_rect,
                        .style = rs,
                        .style_fields = eff.fields,
                        .hover_t = n->hover_t,
                        .active_t = n->active_t,
                        .content = {.label = label,
                                    .text = tm,
                                    .menu_trigger = true,
                                    .popup_open = open},
                    });

    ui->last_response = r;

    if (!open)
        return false;

    /* open the popup below the trigger */
    flux_rect anchor = n->has_prev ? n->prev_rect : (flux_rect){0, 0, w, h};
    return lens_place_begin(ui, ov_label, menu_panel_opts(&rs, anchor));
}

void lens_menu_end(lens *ui) {
    menu_apply_nav(ui); /* every row of this menu is built by now */
    lens_place_end(ui);
}

/* ------------------------------------------------------------------ */
/*  Submenu                                                            */
/* ------------------------------------------------------------------ */

/* Anchors to the right of the parent item (the last node linked). */
bool lens_submenu_begin(lens *ui, const char *label) {
    const lens_theme *t = &ui->theme;
    lens_style eff = lensi_style_effective(ui);
    float font_size = lensi_style_font_size(&eff, t);
    float padding = lensi_style_padding(&eff, t);
    lens_style_resolved rs = lensi_style_resolve(&eff, t, 0);
    lens_node *parent = ui->last_node; /* the item row this submenu hangs off */
    lens_id id = lensi_gen_widget_id(ui, label);
    lens_node *n = lensi_store_touch(ui, id);
    if (!n)
        return false;
    lensi_link_child(ui, n);
    n->is_container = false;

    lens_text_metrics tm = lensi_text_measure_label(ui, label, font_size, 0.0f);
    float w = tm.width + 2.0f * padding;
    float h = font_size + 2.0f * padding;
    n->measured = (flux_point){w, h};

    lens_response r = lensi_interact(ui, n, true, false);
    if (r.hovered)
        ui->cursor_hint = LENS_CURSOR_POINTER;
    uint32_t sem_flags = (r.focused ? LENS_A11Y_FOCUSED : 0);
    lensi_node_semantics(ui, n, LENS_ROLE_MENUITEM, label, NULL, sem_flags);
    if (r.focused)
        menu_arrow_nav(ui, n);
    lens_menu_item_state *st = lens_node_state(n, sizeof *st);

    /* The popup gets its own id ("label##menu") distinct from the item row:
     * under the one-tree model (ADR-0060) the row stays a flow child of the
     * parent menu while the popup is an ABS child — sharing the node would
     * yank the row out of the flow. */
    char sub_label[80];
    lens_id sub_id = menu_popup_id(ui, label, sub_label, sizeof sub_label);

    /* Dwell: accumulate hover time; open after ~0.25s. Reset on leave. */
    float dt = ui->input.dt_seconds;
    if (r.hovered)
        st->dwell += dt;
    else
        st->dwell = 0.0f;
    bool open = lensi_place_is_open_id(ui, sub_id) || st->dwell > 0.25f;
    if (open && !lensi_place_is_open_id(ui, sub_id))
        lensi_place_open_id_pub(ui, sub_id, true);

    n->hover_t = lensi_approach(ui, n->hover_t, r.hovered ? 1.f : 0.f, dt, 15.f);

    /* emit — through the replaceable skin (ADR-0059) */
    lensi_skin_emit(ui, n,
                    &(lens_widget_record){
                        .kind = LENS_WIDGET_MENU_ITEM,
                        .state = r.state,
                        .bounds = {0, 0, w, h},
                        .last_bounds = n->prev_rect,
                        .style = rs,
                        .style_fields = eff.fields,
                        .hover_t = n->hover_t,
                        .active_t = n->active_t,
                        .content = {.label = label, .text = tm, .submenu = true},
                    });

    ui->last_response = r;
    if (!open)
        return false;

    /* anchor to the right of the parent item's last-frame rect */
    flux_rect anchor = {0, 0, 1, h};
    if (parent && parent->has_prev)
        anchor = (flux_rect){parent->prev_rect.x + parent->prev_rect.w, parent->prev_rect.y, 1,
                             parent->prev_rect.h};
    return lens_place_begin(ui, sub_label, menu_panel_opts(&rs, anchor));
}

void lens_submenu_end(lens *ui) {
    menu_apply_nav(ui);
    lens_place_end(ui);
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
    lensi_place_open_id_pub(ui, lid, true);
    (void)owner_rect;
}

bool lens_context_menu_begin(lens *ui, const char *id) {
    if (!ui || !id)
        return false;
    lens_style eff = lensi_style_effective(ui);
    lens_style_resolved rs = lensi_style_resolve(&eff, &ui->theme, 0);
    lens_id lid = lensi_gen_widget_id(ui, id);
    if (!lensi_place_is_open_id(ui, lid))
        return false;
    lens_node *n = lensi_store_find(ui, lid);
    flux_point at = {0, 0};
    if (n) {
        lens_ctxmenu_state *st = lens_node_state(n, sizeof *st);
        at = st->at;
    }
    flux_rect anchor = {at.x, at.y, 1, 1};
    if (lens_place_begin(ui, id, menu_panel_opts(&rs, anchor))) {
        if (ui->last_node)
            lensi_node_semantics(ui, ui->last_node, LENS_ROLE_MENU, id, NULL, 0);
        return true;
    }
    return false;
}

void lens_context_menu_end(lens *ui) {
    menu_apply_nav(ui);
    lens_place_end(ui);
}
