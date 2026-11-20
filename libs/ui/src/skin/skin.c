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

optics_ui_skin_fn optics_ui_default_skin(optics_ui_widget_kind kind) {
    switch (kind) {
    case OPTICS_UI_WIDGET_LABEL:
        return lensi_skin_label;
    case OPTICS_UI_WIDGET_ICON:
        return lensi_skin_icon;
    case OPTICS_UI_WIDGET_IMAGE:
        return lensi_skin_image;
    case OPTICS_UI_WIDGET_SEPARATOR:
        return lensi_skin_separator;
    case OPTICS_UI_WIDGET_BUTTON:
        return lensi_skin_button;
    case OPTICS_UI_WIDGET_CHECKBOX:
        return lensi_skin_checkbox;
    case OPTICS_UI_WIDGET_SELECTABLE:
        return lensi_skin_selectable;
    case OPTICS_UI_WIDGET_SLIDER:
        return lensi_skin_slider;
    case OPTICS_UI_WIDGET_TEXTEDIT:
        return lensi_skin_textedit;
    default:
        return nullptr;
    }
}

void optics_ui_set_skin(optics_ui *ui, optics_ui_widget_kind kind, optics_ui_skin_fn fn) {
    if (!ui)
        return;
    /* Host-reserved range (ADR-0073): routed to the side table, never the
     * count-sized library arrays. */
    if ((uint32_t)kind >= (uint32_t)OPTICS_UI_WIDGET_KIND_USER_BASE) {
        uint32_t i = (uint32_t)kind - (uint32_t)OPTICS_UI_WIDGET_KIND_USER_BASE;
        if (i >= LENSI_USER_SKIN_MAX)
            return;
        ui->user_skins[i] = fn;
        if (fn) {
            ui->user_skins_userdata[i] = nullptr;
            ui->user_skins_user[i] = nullptr;
        }
        return;
    }
    if (kind >= OPTICS_UI_WIDGET_KIND_COUNT)
        return;
    ui->skins[kind] = fn; /* NULL restores the built-in default */
    if (fn) {
        /* The plain form supersedes a userdata registration for this kind;
         * clearing it keeps "last registration wins" predictable. */
        ui->skins_userdata[kind] = nullptr;
        ui->skins_user[kind] = nullptr;
    }
}

void optics_ui_set_skin_userdata(optics_ui *ui, optics_ui_widget_kind kind,
                                 optics_ui_skin_userdata_fn fn, void *user) {
    if (!ui)
        return;
    if ((uint32_t)kind >= (uint32_t)OPTICS_UI_WIDGET_KIND_USER_BASE) {
        uint32_t i = (uint32_t)kind - (uint32_t)OPTICS_UI_WIDGET_KIND_USER_BASE;
        if (i >= LENSI_USER_SKIN_MAX)
            return;
        ui->user_skins_userdata[i] = fn;
        ui->user_skins_user[i] = user;
        if (fn)
            ui->user_skins[i] = nullptr;
        return;
    }
    if (kind >= OPTICS_UI_WIDGET_KIND_COUNT)
        return;
    ui->skins_userdata[kind] = fn;
    ui->skins_user[kind] = user;
    if (fn)
        ui->skins[kind] = nullptr; /* userdata form wins; see internal.h */
}

void lensi_skin_emit(optics_ui *ui, optics_ui_node *n, const optics_ui_widget_record *rec) {
    /* User-kind lookup first (ADR-0073): no built-in default exists there,
     * so a missing host skin means no emission — by design. */
    if ((uint32_t)rec->kind >= (uint32_t)OPTICS_UI_WIDGET_KIND_USER_BASE) {
        uint32_t i = (uint32_t)rec->kind - (uint32_t)OPTICS_UI_WIDGET_KIND_USER_BASE;
        if (i >= LENSI_USER_SKIN_MAX)
            return;
        optics_ui_skin_userdata_fn ufn = ui->user_skins_userdata[i];
        if (ufn) {
            ufn(ui, n, rec, ui->user_skins_user[i]);
            return;
        }
        optics_ui_skin_fn fn = ui->user_skins[i];
        if (fn)
            fn(ui, n, rec);
        return;
    }
    if (rec->kind < OPTICS_UI_WIDGET_KIND_COUNT) {
        optics_ui_skin_userdata_fn ufn = ui->skins_userdata[rec->kind];
        if (ufn) {
            ufn(ui, n, rec, ui->skins_user[rec->kind]);
            return;
        }
    }
    optics_ui_skin_fn fn = nullptr;
    if (rec->kind < OPTICS_UI_WIDGET_KIND_COUNT)
        fn = ui->skins[rec->kind];
    if (!fn)
        fn = optics_ui_default_skin(rec->kind);
    if (fn)
        fn(ui, n, rec);
}

void optics_ui_skin_emit_user(optics_ui *ui, const optics_ui_skin_emit_user_opts *opts) {
    if (!ui || !opts || !opts->node)
        return;
    if ((uint32_t)opts->kind < (uint32_t)OPTICS_UI_WIDGET_KIND_USER_BASE)
        return; /* built-ins own their emission; see lens.h */
    optics_ui_widget_record rec = opts->rec;
    rec.kind = opts->kind;
    lensi_skin_emit(ui, opts->node, &rec);
}

/* ---- per-node scratch (ADR-0061 item 9) ---------------------------- */
/* Mechanism, not animation: the library stores four floats per node so a
 * caller-owned skin can carry its own state; it never integrates anything
 * itself. */

float *optics_ui_skin_scratch(optics_ui *ui, optics_ui_node *node) {
    (void)ui; /* the node already knows its owning context */
    return node ? node->skin_scratch : nullptr;
}

/* ---- public emission seam (the skin's pen) --------------------------
 * Thin wrappers over lensi_drawlist_push; the rect/text conventions
 * (zero-size spans, negative text rel.w/h centring) are documented at the
 * declarations in <lens/lens.h>. Built-in skins call lensi_drawlist_push
 * directly — they predate the seam and need its full field set. */

void optics_ui_skin_rect(optics_ui *ui, const optics_ui_skin_rect_opts *opts) {
    if (!opts)
        return;
    lensi_drawlist_push(ui, opts->node,
                        (optics_ui_draw_cmd){.kind = OPTICS_UI_DRAW_RECT,
                                             .rel = opts->rel,
                                             .color = opts->color,
                                             .radius = opts->radius});
}

void optics_ui_skin_border(optics_ui *ui, const optics_ui_skin_border_opts *opts) {
    if (!opts)
        return;
    lensi_drawlist_push(ui, opts->node,
                        (optics_ui_draw_cmd){.kind = OPTICS_UI_DRAW_BORDER,
                                             .rel = opts->rel,
                                             .color = opts->color,
                                             .radius = opts->radius,
                                             .width = opts->width});
}

void optics_ui_skin_text(optics_ui *ui, const optics_ui_skin_text_opts *opts) {
    if (!opts)
        return;
    lensi_drawlist_push(ui, opts->node,
                        (optics_ui_draw_cmd){.kind = OPTICS_UI_DRAW_TEXT,
                                             .rel = opts->rel,
                                             .color = opts->color,
                                             .text = opts->utf8,
                                             .text_size = opts->size_px,
                                             .text_weight = opts->weight});
}

void optics_ui_skin_icon(optics_ui *ui, const optics_ui_skin_icon_opts *opts) {
    if (!opts)
        return;
    lensi_drawlist_push(ui, opts->node,
                        (optics_ui_draw_cmd){.kind = OPTICS_UI_DRAW_ICON,
                                             .rel = opts->rel,
                                             .color = opts->color,
                                             .width = opts->stroke,
                                             .icon_id = opts->icon});
}

void optics_ui_skin_clip_push(optics_ui *ui, const optics_ui_skin_clip_push_opts *opts) {
    if (!opts)
        return;
    lensi_drawlist_push(ui, opts->node,
                        (optics_ui_draw_cmd){.kind = OPTICS_UI_DRAW_CLIP_PUSH, .rel = opts->rel});
}

void optics_ui_skin_clip_pop(optics_ui *ui, optics_ui_node *node) {
    lensi_drawlist_push(ui, node,
                        (optics_ui_draw_cmd){.kind = OPTICS_UI_DRAW_CLIP_POP, .rel = {0, 0, 0, 0}});
}
