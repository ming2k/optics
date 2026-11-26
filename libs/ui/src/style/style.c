/* style.c — the style cascade + resolution (ADR-0058, amended ADR-0061).
 *
 * The cascade (ADR-0061 item 2) is one fixed per-field order:
 *
 *      per-call style (optics_ui_box.style, staged as ui->next_style)
 *        > nearest enclosing scope (the optics_ui_push_style stack, folded
 *          bottom-up so the innermost scope wins per field)
 *        > theme
 *
 * optics_ui__style_merge overlays one sparse style on another per field;
 * optics_ui__style_effective folds the scope stack and drains the staged
 * per-call style, producing the *effective* style a widget then measures
 * and resolves against. All three are pure data transforms — the only
 * side effect anywhere is optics_ui__style_effective clearing ui->next_style.
 *
 * optics_ui__style_resolve turns the effective style into a fully concrete
 * optics_ui_style_resolved. Its derivation order (the ONLY adjustment order;
 * widgets must not re-adjust):
 *
 *   1. Fallback — every unset slot takes its theme token:
 *      bg←color_bg, bg_hover←color_hover, bg_pressed←color_active,
 *      fg←color_fg, border←color_border, accent←color_accent, and the
 *      geometry slots from their same-named tokens. `disabled` is always
 *      color_disabled (optics_ui_style has no instance slot for it). The
 *      outline atoms have no theme token: unset resolves to
 *      transparent/0 (no contour).
 *
 *   2. Derivation — hover lift, then pressed deepen, for a cascaded-set
 *      base only: when `bg` is set (by per-call or scope) but `bg_hover`
 *      is not, bg_hover mixes OPTICS_UI_STYLE_HOVER_LIFT of the resolved
 *      foreground into bg; when `bg` is set but `bg_pressed` is not,
 *      bg_pressed mixes OPTICS_UI_STYLE_PRESSED_DEPTH of the foreground into
 *      the *base* bg (not the hover value). This is what makes a
 *      one-colour override self-sufficient: set `bg`, get hover/pressed
 *      feedback for free. Theme-sourced slots are never derived over —
 *      color_hover and color_active are already explicit design tokens.
 *
 *   3. Disabled dim — LAST, so it applies on top of the hover/pressed
 *      adjustments: with OPTICS_UI_STATE_DISABLED set, every cascade-sourced
 *      colour slot (set directly or derived in step 2) blends
 *      OPTICS_UI_STYLE_DISABLED_DIM toward `disabled`. Theme-sourced slots are
 *      not dimmed: the theme already carries a designed disabled token and
 *      widgets read `resolved.disabled` for their disabled branches. The
 *      outline atoms are state-independent decoration: never derived,
 *      never dimmed.
 *
 * Consequence, and the reason for the "cascade-only" rules in steps 2–3:
 * with an empty per-call style and an empty scope stack the output is the
 * verbatim theme for EVERY state, so widgets that switched from reading
 * `optics_ui_theme` to reading the resolved style render pixel-identically by
 * construction.
 */

#include "../internal.h"

/* Per-field overlay: `over` wins where its mask is set, `base` fills the
 * rest. This one function IS the cascade's "per-field" rule — there is no
 * whole-style replacement anywhere. */
optics_ui_style optics_ui__style_merge(const optics_ui_style *base, const optics_ui_style *over) {
    optics_ui_style out = *base;
    uint32_t f = over->fields;
    if (f & OPTICS_UI_STYLE_BG)
        out.bg = over->bg;
    if (f & OPTICS_UI_STYLE_BG_HOVER)
        out.bg_hover = over->bg_hover;
    if (f & OPTICS_UI_STYLE_BG_PRESSED)
        out.bg_pressed = over->bg_pressed;
    if (f & OPTICS_UI_STYLE_FG)
        out.fg = over->fg;
    if (f & OPTICS_UI_STYLE_BORDER)
        out.border = over->border;
    if (f & OPTICS_UI_STYLE_ACCENT)
        out.accent = over->accent;
    if (f & OPTICS_UI_STYLE_CORNER_RADIUS)
        out.corner_radius = over->corner_radius;
    if (f & OPTICS_UI_STYLE_BORDER_WIDTH)
        out.border_width = over->border_width;
    if (f & OPTICS_UI_STYLE_PADDING)
        out.padding = over->padding;
    if (f & OPTICS_UI_STYLE_GAP)
        out.gap = over->gap;
    if (f & OPTICS_UI_STYLE_FONT_SIZE)
        out.font_size = over->font_size;
    if (f & OPTICS_UI_STYLE_ICON_SIZE)
        out.icon_size = over->icon_size;
    if (f & OPTICS_UI_STYLE_OUTLINE_COLOR)
        out.outline_color = over->outline_color;
    if (f & OPTICS_UI_STYLE_OUTLINE_WIDTH)
        out.outline_width = over->outline_width;
    out.fields = base->fields | over->fields;
    return out;
}

/* The folded scope stack: bottom entry first, so the NEAREST enclosing
 * scope wins per field (ADR-0061). */
optics_ui_style optics_ui__style_scope_merged(const optics_ui *ui) {
    optics_ui_style acc = optics_ui_style_init();
    for (uint32_t i = 0; i < ui->style_top; i++)
        acc = optics_ui__style_merge(&acc, &ui->style_stack[i]);
    return acc;
}

/* The effective style for the widget currently being built: the staged
 * per-call style (optics_ui_box.style) over the folded scope stack. Drains
 * ui->next_style like the other pending modifiers. */
optics_ui_style optics_ui__style_effective(optics_ui *ui) {
    optics_ui_style inst = ui->next_style;
    ui->next_style = optics_ui_style_init();
    optics_ui_style scope = optics_ui__style_scope_merged(ui);
    return optics_ui__style_merge(&scope, &inst);
}

optics_ui_style_resolved optics_ui__style_resolve(optics_ui *ui, const optics_ui_style *eff,
                                                  const optics_ui_theme *theme, uint32_t state) {
    uint32_t f = eff ? eff->fields : 0;
    optics_ui_style_resolved r;

    /* 1. fallback: unset slots take the theme token */
    r.bg = (f & OPTICS_UI_STYLE_BG) ? eff->bg : theme->color_bg;
    r.bg_hover = (f & OPTICS_UI_STYLE_BG_HOVER) ? eff->bg_hover : theme->color_hover;
    r.bg_pressed = (f & OPTICS_UI_STYLE_BG_PRESSED) ? eff->bg_pressed : theme->color_active;
    r.fg = (f & OPTICS_UI_STYLE_FG) ? eff->fg : theme->color_fg;
    r.border = (f & OPTICS_UI_STYLE_BORDER) ? eff->border : theme->color_border;
    r.accent = (f & OPTICS_UI_STYLE_ACCENT) ? eff->accent : theme->color_accent;
    r.disabled = theme->color_disabled;
    r.corner_radius =
        (f & OPTICS_UI_STYLE_CORNER_RADIUS) ? eff->corner_radius : theme->corner_radius;
    r.border_width = (f & OPTICS_UI_STYLE_BORDER_WIDTH) ? eff->border_width : theme->border_width;
    r.padding = optics_ui__style_padding(eff, theme);
    r.gap = (f & OPTICS_UI_STYLE_GAP) ? eff->gap : theme->gap;
    r.font_size = optics_ui__style_font_size(ui, eff, theme); /* folds in text_scale */
    float theme_icon_size = theme->icon_size > 0.0f ? theme->icon_size : r.font_size;
    r.icon_size = (f & OPTICS_UI_STYLE_ICON_SIZE) ? eff->icon_size : theme_icon_size;

    /* No theme tokens exist for the outline atoms: unset means none. */
    r.outline_color = (f & OPTICS_UI_STYLE_OUTLINE_COLOR) ? eff->outline_color : 0;
    r.outline_width = (f & OPTICS_UI_STYLE_OUTLINE_WIDTH) ? eff->outline_width : 0.0f;

    /* Slots the cascade is responsible for — set directly, or derived
     * below — tracked so the disabled dim (step 3) touches only those. */
    uint32_t sourced =
        f & (OPTICS_UI_STYLE_BG | OPTICS_UI_STYLE_BG_HOVER | OPTICS_UI_STYLE_BG_PRESSED |
             OPTICS_UI_STYLE_FG | OPTICS_UI_STYLE_BORDER | OPTICS_UI_STYLE_ACCENT);

    /* 2. derivation: hover lift, then pressed deepen, off a cascaded bg */
    if ((f & OPTICS_UI_STYLE_BG) && !(f & OPTICS_UI_STYLE_BG_HOVER)) {
        r.bg_hover = optics_ui__lerp_color(r.bg, r.fg, OPTICS_UI_STYLE_HOVER_LIFT);
        sourced |= OPTICS_UI_STYLE_BG_HOVER;
    }
    if ((f & OPTICS_UI_STYLE_BG) && !(f & OPTICS_UI_STYLE_BG_PRESSED)) {
        r.bg_pressed = optics_ui__lerp_color(r.bg, r.fg, OPTICS_UI_STYLE_PRESSED_DEPTH);
        sourced |= OPTICS_UI_STYLE_BG_PRESSED;
    }

    /* 3. disabled dim, last: only cascade-sourced colours blend toward
     * the theme's designed disabled colour. */
    if (state & OPTICS_UI_STATE_DISABLED) {
        if (sourced & OPTICS_UI_STYLE_BG)
            r.bg = optics_ui__lerp_color(r.bg, r.disabled, OPTICS_UI_STYLE_DISABLED_DIM);
        if (sourced & OPTICS_UI_STYLE_BG_HOVER)
            r.bg_hover =
                optics_ui__lerp_color(r.bg_hover, r.disabled, OPTICS_UI_STYLE_DISABLED_DIM);
        if (sourced & OPTICS_UI_STYLE_BG_PRESSED)
            r.bg_pressed =
                optics_ui__lerp_color(r.bg_pressed, r.disabled, OPTICS_UI_STYLE_DISABLED_DIM);
        if (sourced & OPTICS_UI_STYLE_FG)
            r.fg = optics_ui__lerp_color(r.fg, r.disabled, OPTICS_UI_STYLE_DISABLED_DIM);
        if (sourced & OPTICS_UI_STYLE_BORDER)
            r.border = optics_ui__lerp_color(r.border, r.disabled, OPTICS_UI_STYLE_DISABLED_DIM);
        if (sourced & OPTICS_UI_STYLE_ACCENT)
            r.accent = optics_ui__lerp_color(r.accent, r.disabled, OPTICS_UI_STYLE_DISABLED_DIM);
    }

    return r;
}
