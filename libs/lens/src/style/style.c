/* style.c — the style cascade + resolution (ADR-0058, amended ADR-0061).
 *
 * The cascade (ADR-0061 item 2) is one fixed per-field order:
 *
 *      per-call style (lens_box.style, staged as ui->next_style)
 *        > nearest enclosing scope (the lens_push_style stack, folded
 *          bottom-up so the innermost scope wins per field)
 *        > theme
 *
 * lensi_style_merge overlays one sparse style on another per field;
 * lensi_style_effective folds the scope stack and drains the staged
 * per-call style, producing the *effective* style a widget then measures
 * and resolves against. All three are pure data transforms — the only
 * side effect anywhere is lensi_style_effective clearing ui->next_style.
 *
 * lensi_style_resolve turns the effective style into a fully concrete
 * lens_style_resolved. Its derivation order (the ONLY adjustment order;
 * widgets must not re-adjust):
 *
 *   1. Fallback — every unset slot takes its theme token:
 *      bg←color_bg, bg_hover←color_hover, bg_pressed←color_active,
 *      fg←color_fg, border←color_border, accent←color_accent, and the
 *      geometry slots from their same-named tokens. `disabled` is always
 *      color_disabled (lens_style has no instance slot for it). The
 *      outline atoms have no theme token: unset resolves to
 *      transparent/0 (no contour).
 *
 *   2. Derivation — hover lift, then pressed deepen, for a cascaded-set
 *      base only: when `bg` is set (by per-call or scope) but `bg_hover`
 *      is not, bg_hover mixes LENSI_STYLE_HOVER_LIFT of the resolved
 *      foreground into bg; when `bg` is set but `bg_pressed` is not,
 *      bg_pressed mixes LENSI_STYLE_PRESSED_DEPTH of the foreground into
 *      the *base* bg (not the hover value). This is what makes a
 *      one-colour override self-sufficient: set `bg`, get hover/pressed
 *      feedback for free. Theme-sourced slots are never derived over —
 *      color_hover and color_active are already explicit design tokens.
 *
 *   3. Disabled dim — LAST, so it applies on top of the hover/pressed
 *      adjustments: with LENS_STATE_DISABLED set, every cascade-sourced
 *      colour slot (set directly or derived in step 2) blends
 *      LENSI_STYLE_DISABLED_DIM toward `disabled`. Theme-sourced slots are
 *      not dimmed: the theme already carries a designed disabled token and
 *      widgets read `resolved.disabled` for their disabled branches. The
 *      outline atoms are state-independent decoration: never derived,
 *      never dimmed.
 *
 * Consequence, and the reason for the "cascade-only" rules in steps 2–3:
 * with an empty per-call style and an empty scope stack the output is the
 * verbatim theme for EVERY state, so widgets that switched from reading
 * `lens_theme` to reading the resolved style render pixel-identically by
 * construction.
 */

#include "../internal.h"

/* Per-field overlay: `over` wins where its mask is set, `base` fills the
 * rest. This one function IS the cascade's "per-field" rule — there is no
 * whole-style replacement anywhere. */
lens_style lensi_style_merge(const lens_style *base, const lens_style *over) {
    lens_style out = *base;
    uint32_t f = over->fields;
    if (f & LENS_STYLE_BG)
        out.bg = over->bg;
    if (f & LENS_STYLE_BG_HOVER)
        out.bg_hover = over->bg_hover;
    if (f & LENS_STYLE_BG_PRESSED)
        out.bg_pressed = over->bg_pressed;
    if (f & LENS_STYLE_FG)
        out.fg = over->fg;
    if (f & LENS_STYLE_BORDER)
        out.border = over->border;
    if (f & LENS_STYLE_ACCENT)
        out.accent = over->accent;
    if (f & LENS_STYLE_CORNER_RADIUS)
        out.corner_radius = over->corner_radius;
    if (f & LENS_STYLE_BORDER_WIDTH)
        out.border_width = over->border_width;
    if (f & LENS_STYLE_PADDING)
        out.padding = over->padding;
    if (f & LENS_STYLE_GAP)
        out.gap = over->gap;
    if (f & LENS_STYLE_FONT_SIZE)
        out.font_size = over->font_size;
    if (f & LENS_STYLE_OUTLINE_COLOR)
        out.outline_color = over->outline_color;
    if (f & LENS_STYLE_OUTLINE_WIDTH)
        out.outline_width = over->outline_width;
    out.fields = base->fields | over->fields;
    return out;
}

/* The folded scope stack: bottom entry first, so the NEAREST enclosing
 * scope wins per field (ADR-0061). */
lens_style lensi_style_scope_merged(const lens *ui) {
    lens_style acc = lens_style_init();
    for (uint32_t i = 0; i < ui->style_top; i++)
        acc = lensi_style_merge(&acc, &ui->style_stack[i]);
    return acc;
}

/* The effective style for the widget currently being built: the staged
 * per-call style (lens_box.style) over the folded scope stack. Drains
 * ui->next_style like the other pending modifiers. */
lens_style lensi_style_effective(lens *ui) {
    lens_style inst = ui->next_style;
    ui->next_style = lens_style_init();
    lens_style scope = lensi_style_scope_merged(ui);
    return lensi_style_merge(&scope, &inst);
}

lens_style_resolved lensi_style_resolve(const lens_style *eff, const lens_theme *theme,
                                        uint32_t state) {
    uint32_t f = eff ? eff->fields : 0;
    lens_style_resolved r;

    /* 1. fallback: unset slots take the theme token */
    r.bg = (f & LENS_STYLE_BG) ? eff->bg : theme->color_bg;
    r.bg_hover = (f & LENS_STYLE_BG_HOVER) ? eff->bg_hover : theme->color_hover;
    r.bg_pressed = (f & LENS_STYLE_BG_PRESSED) ? eff->bg_pressed : theme->color_active;
    r.fg = (f & LENS_STYLE_FG) ? eff->fg : theme->color_fg;
    r.border = (f & LENS_STYLE_BORDER) ? eff->border : theme->color_border;
    r.accent = (f & LENS_STYLE_ACCENT) ? eff->accent : theme->color_accent;
    r.disabled = theme->color_disabled;
    r.corner_radius = (f & LENS_STYLE_CORNER_RADIUS) ? eff->corner_radius : theme->corner_radius;
    r.border_width = (f & LENS_STYLE_BORDER_WIDTH) ? eff->border_width : theme->border_width;
    r.padding = lensi_style_padding(eff, theme);
    r.gap = (f & LENS_STYLE_GAP) ? eff->gap : theme->gap;
    r.font_size = lensi_style_font_size(eff, theme);
    /* No theme tokens exist for the outline atoms: unset means none. */
    r.outline_color = (f & LENS_STYLE_OUTLINE_COLOR) ? eff->outline_color : 0;
    r.outline_width = (f & LENS_STYLE_OUTLINE_WIDTH) ? eff->outline_width : 0.0f;

    /* Slots the cascade is responsible for — set directly, or derived
     * below — tracked so the disabled dim (step 3) touches only those. */
    uint32_t sourced = f & (LENS_STYLE_BG | LENS_STYLE_BG_HOVER | LENS_STYLE_BG_PRESSED |
                            LENS_STYLE_FG | LENS_STYLE_BORDER | LENS_STYLE_ACCENT);

    /* 2. derivation: hover lift, then pressed deepen, off a cascaded bg */
    if ((f & LENS_STYLE_BG) && !(f & LENS_STYLE_BG_HOVER)) {
        r.bg_hover = lensi_lerp_color(r.bg, r.fg, LENSI_STYLE_HOVER_LIFT);
        sourced |= LENS_STYLE_BG_HOVER;
    }
    if ((f & LENS_STYLE_BG) && !(f & LENS_STYLE_BG_PRESSED)) {
        r.bg_pressed = lensi_lerp_color(r.bg, r.fg, LENSI_STYLE_PRESSED_DEPTH);
        sourced |= LENS_STYLE_BG_PRESSED;
    }

    /* 3. disabled dim, last: only cascade-sourced colours blend toward
     * the theme's designed disabled colour. */
    if (state & LENS_STATE_DISABLED) {
        if (sourced & LENS_STYLE_BG)
            r.bg = lensi_lerp_color(r.bg, r.disabled, LENSI_STYLE_DISABLED_DIM);
        if (sourced & LENS_STYLE_BG_HOVER)
            r.bg_hover = lensi_lerp_color(r.bg_hover, r.disabled, LENSI_STYLE_DISABLED_DIM);
        if (sourced & LENS_STYLE_BG_PRESSED)
            r.bg_pressed = lensi_lerp_color(r.bg_pressed, r.disabled, LENSI_STYLE_DISABLED_DIM);
        if (sourced & LENS_STYLE_FG)
            r.fg = lensi_lerp_color(r.fg, r.disabled, LENSI_STYLE_DISABLED_DIM);
        if (sourced & LENS_STYLE_BORDER)
            r.border = lensi_lerp_color(r.border, r.disabled, LENSI_STYLE_DISABLED_DIM);
        if (sourced & LENS_STYLE_ACCENT)
            r.accent = lensi_lerp_color(r.accent, r.disabled, LENSI_STYLE_DISABLED_DIM);
    }

    return r;
}
