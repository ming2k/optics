/* skin.c — skin dispatch, registry, and the public emission seam
 * (ADR-0059).
 *
 * A migrated widget ends its build by packing a lens_widget_record and
 * calling lensi_skin_emit; the skin is the only code that writes draw
 * commands for the widget's chrome. Replacement is context-wide
 * (lens_set_skin); NULL restores the built-in default, which is the
 * pre-skin emission code moved verbatim — default rendering is
 * unchanged. */

#include "../internal.h"

lens_skin_fn lens_default_skin(lens_widget_kind kind) {
    switch (kind) {
    case LENS_WIDGET_LABEL:
        return lensi_skin_label;
    case LENS_WIDGET_ICON:
        return lensi_skin_icon;
    case LENS_WIDGET_IMAGE:
        return lensi_skin_image;
    case LENS_WIDGET_SEPARATOR:
        return lensi_skin_separator;
    case LENS_WIDGET_BUTTON:
        return lensi_skin_button;
    case LENS_WIDGET_CHECKBOX:
        return lensi_skin_checkbox;
    case LENS_WIDGET_SELECTABLE:
        return lensi_skin_selectable;
    case LENS_WIDGET_SLIDER:
        return lensi_skin_slider;
    case LENS_WIDGET_TEXTEDIT:
        return lensi_skin_textedit;
    default:
        return NULL;
    }
}

void lens_set_skin(lens *ui, lens_widget_kind kind, lens_skin_fn fn) {
    if (!ui)
        return;
    /* Host-reserved range (ADR-0073): routed to the side table, never the
     * count-sized library arrays. */
    if ((uint32_t)kind >= (uint32_t)LENS_WIDGET_KIND_USER_BASE) {
        uint32_t i = (uint32_t)kind - (uint32_t)LENS_WIDGET_KIND_USER_BASE;
        if (i >= LENSI_USER_SKIN_MAX)
            return;
        ui->user_skins[i] = fn;
        if (fn) {
            ui->user_skins_userdata[i] = NULL;
            ui->user_skins_user[i] = NULL;
        }
        return;
    }
    if (kind >= LENS_WIDGET_KIND_COUNT)
        return;
    ui->skins[kind] = fn; /* NULL restores the built-in default */
    if (fn) {
        /* The plain form supersedes a userdata registration for this kind;
         * clearing it keeps "last registration wins" predictable. */
        ui->skins_userdata[kind] = NULL;
        ui->skins_user[kind] = NULL;
    }
}

void lens_set_skin_userdata(lens *ui, lens_widget_kind kind, lens_skin_userdata_fn fn, void *user) {
    if (!ui)
        return;
    if ((uint32_t)kind >= (uint32_t)LENS_WIDGET_KIND_USER_BASE) {
        uint32_t i = (uint32_t)kind - (uint32_t)LENS_WIDGET_KIND_USER_BASE;
        if (i >= LENSI_USER_SKIN_MAX)
            return;
        ui->user_skins_userdata[i] = fn;
        ui->user_skins_user[i] = user;
        if (fn)
            ui->user_skins[i] = NULL;
        return;
    }
    if (kind >= LENS_WIDGET_KIND_COUNT)
        return;
    ui->skins_userdata[kind] = fn;
    ui->skins_user[kind] = user;
    if (fn)
        ui->skins[kind] = NULL; /* userdata form wins; see internal.h */
}

void lensi_skin_emit(lens *ui, lens_node *n, const lens_widget_record *rec) {
    /* User-kind lookup first (ADR-0073): no built-in default exists there,
     * so a missing host skin means no emission — by design. */
    if ((uint32_t)rec->kind >= (uint32_t)LENS_WIDGET_KIND_USER_BASE) {
        uint32_t i = (uint32_t)rec->kind - (uint32_t)LENS_WIDGET_KIND_USER_BASE;
        if (i >= LENSI_USER_SKIN_MAX)
            return;
        lens_skin_userdata_fn ufn = ui->user_skins_userdata[i];
        if (ufn) {
            ufn(ui, n, rec, ui->user_skins_user[i]);
            return;
        }
        lens_skin_fn fn = ui->user_skins[i];
        if (fn)
            fn(ui, n, rec);
        return;
    }
    if (rec->kind < LENS_WIDGET_KIND_COUNT) {
        lens_skin_userdata_fn ufn = ui->skins_userdata[rec->kind];
        if (ufn) {
            ufn(ui, n, rec, ui->skins_user[rec->kind]);
            return;
        }
    }
    lens_skin_fn fn = NULL;
    if (rec->kind < LENS_WIDGET_KIND_COUNT)
        fn = ui->skins[rec->kind];
    if (!fn)
        fn = lens_default_skin(rec->kind);
    if (fn)
        fn(ui, n, rec);
}

void lens_skin_emit_user(lens *ui, lens_node *node, lens_widget_kind kind, lens_widget_record rec) {
    if (!ui || !node)
        return;
    if ((uint32_t)kind < (uint32_t)LENS_WIDGET_KIND_USER_BASE)
        return; /* built-ins own their emission; see lens.h */
    rec.kind = kind;
    lensi_skin_emit(ui, node, &rec);
}

/* ---- per-node scratch (ADR-0061 item 9) ---------------------------- */
/* Mechanism, not animation: the library stores four floats per node so a
 * caller-owned skin can carry its own state; it never integrates anything
 * itself. */

float *lens_skin_scratch(lens *ui, lens_node *node) {
    (void)ui; /* the node already knows its owning context */
    return node ? node->skin_scratch : NULL;
}

/* ---- public emission seam (the skin's pen) --------------------------
 * Thin wrappers over lensi_drawlist_push; the rect/text conventions
 * (zero-size spans, negative text rel.w/h centring) are documented at the
 * declarations in <lens/lens.h>. Built-in skins call lensi_drawlist_push
 * directly — they predate the seam and need its full field set. */

void lens_skin_rect(lens *ui, lens_node *node, flux_rect rel, flux_color color, float radius) {
    lensi_drawlist_push(
        ui, node,
        (lens_draw_cmd){.kind = LENS_DRAW_RECT, .rel = rel, .color = color, .radius = radius});
}

void lens_skin_border(lens *ui, lens_node *node, flux_rect rel, flux_color color, float radius,
                      float width) {
    lensi_drawlist_push(ui, node,
                        (lens_draw_cmd){.kind = LENS_DRAW_BORDER,
                                        .rel = rel,
                                        .color = color,
                                        .radius = radius,
                                        .width = width});
}

void lens_skin_text(lens *ui, lens_node *node, flux_rect rel, flux_color color, const char *utf8,
                    float size_px, float weight) {
    lensi_drawlist_push(ui, node,
                        (lens_draw_cmd){.kind = LENS_DRAW_TEXT,
                                        .rel = rel,
                                        .color = color,
                                        .text = utf8,
                                        .text_size = size_px,
                                        .text_weight = weight});
}

void lens_skin_icon(lens *ui, lens_node *node, flux_rect rel, flux_color color, float stroke,
                    lens_icon_id icon) {
    lensi_drawlist_push(
        ui, node,
        (lens_draw_cmd){
            .kind = LENS_DRAW_ICON, .rel = rel, .color = color, .width = stroke, .icon_id = icon});
}

void lens_skin_clip_push(lens *ui, lens_node *node, flux_rect rel) {
    lensi_drawlist_push(ui, node, (lens_draw_cmd){.kind = LENS_DRAW_CLIP_PUSH, .rel = rel});
}

void lens_skin_clip_pop(lens *ui, lens_node *node) {
    lensi_drawlist_push(ui, node, (lens_draw_cmd){.kind = LENS_DRAW_CLIP_POP, .rel = {0, 0, 0, 0}});
}
