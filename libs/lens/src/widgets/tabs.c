/* tabs.c — horizontal tab bar (ADR-0031). Skin migration: ADR-0061 item 8.
 *
 * This file keeps identity, measuring, interaction, accessibility, and the
 * strip's structural layout (equal_width, the content-derived minimum
 * height). ALL emission moved behind the skin seam: one LENS_WIDGET_TABS
 * record per strip per frame, emitted from lens_tabs_end. The default skin
 * (skin/tabs.c) draws the neutral static indicator — theme accent, fixed
 * thickness, zero animation; the spring physics that used to live here is
 * a caller-owned recipe now (examples/showcase/tabs_spring_skin.c).
 *
 * Styling resolves at the STRIP level: lens_tabs_begin[_ex] drains the
 * cascade-effective style and the strip record carries it. Per-tab staged
 * styles are drained and ignored by lens_tab (there is no tab descriptor
 * form) — style the strip, not the tab. */

#include "../internal.h"

/* Default indicator geometry constants (the neutral default's fixed
 * affordance: accent bar under the active tab). */
#define LENSI_TAB_INDICATOR_THICKNESS 3.0f
#define LENSI_TAB_INDICATOR_GAP 2.0f

typedef struct lens_tabs_state {
    int *active;
    uint32_t next_index;
    bool equal_width;
    lens_style eff; /* cascade-effective style captured at begin */
    /* This frame's per-tab data for the skin record. Arena-backed and
     * reassigned every lens_tabs_begin — frame-borrowed like the record. */
    lens_tab_item *items;
    uint32_t item_cap;
} lens_tabs_state;

/* Copy the caller's label into the per-frame arena: the strip skin reads
 * items[] at lens_tabs_end, after the lens_tab call sites (and any
 * stack/binding-local strings they passed) have returned. Same contract as
 * the a11y name copy in semantics.c. */
static const char *tab_arena_label(lens *ui, const char *label) {
    size_t n = strlen(label);
    char *c = flux_arena_alloc(&ui->arena, n + 1);
    if (!c) {
        lensi_set_overflow(ui);
        return "";
    }
    memcpy(c, label, n + 1);
    return c;
}

bool lens_tabs_begin(lens *ui, const char *id, int *active_tab) {
    return lens_tabs_begin_ex(ui, id, active_tab, (lens_tabs_opts){0});
}

bool lens_tabs_begin_ex(lens *ui, const char *id, int *active_tab, lens_tabs_opts opts) {
    lens_id fid = lensi_gen_widget_id(ui, id);
    lens_node *n = lensi_store_touch(ui, fid);
    if (!n)
        return false;
    lensi_link_child(ui, n);
    n->is_container = true;
    n->axis = LENS_ROW;
    n->gap = 0.0f;
    n->pad = 0.0f;
    n->cross = LENS_START;

    lens_tabs_state *ts = lens_node_state(n, sizeof *ts);
    if (ts) {
        ts->active = active_tab;
        ts->next_index = 0;
        ts->equal_width = opts.equal_width;
        ts->eff = lensi_style_effective(ui); /* strip-level cascade drain */
        ts->items = NULL;
        ts->item_cap = 0;
    }
    lensi_open_container_push(ui, n);
    return true;
}

bool lens_tab(lens *ui, const char *label) {
    bool disabled = ui->next_disabled;
    ui->next_disabled = false;
    ui->next_error = false; /* drain so it never leaks to a later widget */
    (void)lensi_style_effective(ui); /* per-tab staged styles are not read:
                                        style the strip (lens_tabs_begin_ex
                                        or a scope around it), not the tab */

    lens_node *parent = lensi_open_container(ui);
    lens_tabs_state *ts = parent ? lens_node_state(parent, sizeof *ts) : NULL;
    int index = ts ? (int)ts->next_index++ : 0;
    bool active = ts && ts->active && *ts->active == index;

    lens_id id = lensi_gen_widget_id(ui, label);
    lens_node *n = lensi_store_touch(ui, id);
    if (!n)
        return false;
    lensi_link_child(ui, n);
    n->is_container = false;

    /* measure — geometry slots from the strip's effective style */
    float font_size = ts ? lensi_style_font_size(&ts->eff, &ui->theme) : ui->theme.font_size;
    float padding = ts ? lensi_style_padding(&ts->eff, &ui->theme) : ui->theme.padding;
    lens_text_metrics tm = lensi_text_measure_label(ui, label, font_size, 0.0f);
    float w = (n->fixed_w > 0) ? n->fixed_w : tm.width + 2.0f * padding;
    float vertical_padding = fmaxf(4.0f, padding * 0.42f);
    float natural_h = tm.height + 2.0f * vertical_padding;
    float h = (n->fixed_h > 0) ? fmaxf(n->fixed_h, natural_h) : natural_h;
    n->measured = (flux_point){w, h};

    lens_response r = lensi_interact(ui, n, true, disabled);
    bool changed = false;
    if (r.clicked && ts && ts->active) {
        *ts->active = index;
        changed = true;
    }

    float dt = ui->input.dt_seconds;
    if (!disabled)
        n->hover_t = lensi_approach(ui, n->hover_t, r.hovered ? 1.f : 0.f, dt, 12.f);

    if (active)
        r.state |= LENS_STATE_SELECTED; /* the current tab (ADR-0058) */
    uint32_t sem_flags = (r.focused ? LENS_A11Y_FOCUSED : 0) | (disabled ? LENS_A11Y_DISABLED : 0) |
                         (active ? (LENS_A11Y_CHECKED | LENS_A11Y_SELECTED) : 0);
    lensi_node_semantics(ui, n, LENS_ROLE_RADIO, label, NULL, sem_flags);

    /* Bank this tab's data for the strip record the skin reads at end. */
    if (ts) {
        if (ts->next_index > ts->item_cap) {
            uint32_t nc = ts->item_cap ? ts->item_cap * 2 : 8;
            lens_tab_item *na = flux_arena_alloc(&ui->arena, nc * sizeof *na);
            if (na) {
                if (ts->items)
                    memcpy(na, ts->items, (ts->next_index - 1) * sizeof *na);
                ts->items = na;
                ts->item_cap = nc;
            } else {
                lensi_set_overflow(ui);
            }
        }
        if (ts->next_index <= ts->item_cap) {
            ts->items[ts->next_index - 1] = (lens_tab_item){
                .label = tab_arena_label(ui, label),
                .text = tm,
                .state = r.state,
                .hover_t = n->hover_t,
                .last_bounds = n->prev_rect,
            };
        }
    }

    ui->last_response = r;
    return changed;
}

void lens_tabs_end(lens *ui) {
    lens_node *tabs = lensi_open_container(ui);
    if (tabs) {
        lens_tabs_state *ts = lens_node_state(tabs, sizeof *ts);
        uint32_t count = ts ? ts->next_index : 0;

        if (ts && ts->equal_width && tabs->first_child) {
            /* Equal width is a strip-level layout policy, not a visual style.
             * Normalize the intrinsic bases before the row solver distributes
             * its remaining space; equal flex weights then produce equal hit
             * targets even when labels have different lengths. */
            float equal_base = 0.0f;
            for (lens_node *child = tabs->first_child; child; child = child->next_sibling) {
                float base = child->fixed_w > 0.0f ? child->fixed_w : child->measured.x;
                equal_base = fmaxf(equal_base, base);
            }
            for (lens_node *child = tabs->first_child; child; child = child->next_sibling) {
                child->measured.x = equal_base;
                child->flex_grow = 1.0f;
            }
        }

        /* The strip must contain its tabs plus the indicator band below
         * them; a smaller host hint is raised, never clipped. */
        float minimum_h = 0.0f;
        for (lens_node *child = tabs->first_child; child; child = child->next_sibling) {
            if (child->measured.y > minimum_h)
                minimum_h = child->measured.y;
        }
        minimum_h += LENSI_TAB_INDICATOR_GAP + LENSI_TAB_INDICATOR_THICKNESS;
        minimum_h += 2.0f * tabs->pad;
        if (tabs->min_h < minimum_h)
            tabs->min_h = minimum_h;
        if (tabs->fixed_h > 0.0f && tabs->fixed_h < minimum_h)
            tabs->fixed_h = minimum_h;

        int active_index = (ts && ts->active) ? *ts->active : 0;
        if (active_index < 0)
            active_index = 0;
        if (count > 0 && active_index >= (int)count)
            active_index = (int)count - 1;

        /* emit — one record per strip, through the replaceable skin
         * (ADR-0059/0061). */
        lens_style_resolved rs =
            lensi_style_resolve(ts ? &ts->eff : NULL, &ui->theme, 0);
        lensi_skin_emit(ui, tabs,
                        &(lens_widget_record){
                            .kind = LENS_WIDGET_TABS,
                            .state = 0,
                            .bounds = {0, 0, 0, 0}, /* strip-local box is solved
                                                       post-build; skins read
                                                       last_bounds + tabs[] */
                            .last_bounds = tabs->prev_rect,
                            .style = rs,
                            .style_fields = ts ? ts->eff.fields : 0,
                            .hover_t = 0.0f,
                            .active_t = 0.0f,
                            .content = {.tabs = ts ? ts->items : NULL,
                                        .tab_count = (int)count,
                                        .active_index = active_index},
                        });
    }
    lensi_open_container_pop(ui);
}
