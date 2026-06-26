/* app_shell.h — small palette helpers for the example app shells.
 *
 * lens's draw list paints rects and borders; with text rendering
 * served through flux core, the way for an example to *look*
 * like an app instead of a stack of widgets is to compose painted
 * panels — toolbars, sidebars, cards, status bars. Those panels are
 * just `lens_row_ex` / `lens_column_ex` containers with `opts.bg` set, so
 * this header is only a palette: tones derived from the active theme.
 */
#ifndef APP_SHELL_H
#define APP_SHELL_H

#include <lens/lens.h>
#include <stdint.h>

static inline flux_color shell_shift(flux_color c, int d) {
    uint8_t r, g, b, a;
    flux_color_unpack(c, &r, &g, &b, &a);
    int rr = (int)r + d;
    if (rr < 0)
        rr = 0;
    if (rr > 255)
        rr = 255;
    int gg = (int)g + d;
    if (gg < 0)
        gg = 0;
    if (gg > 255)
        gg = 255;
    int bb = (int)b + d;
    if (bb < 0)
        bb = 0;
    if (bb > 255)
        bb = 255;
    return flux_color_rgba((uint8_t)rr, (uint8_t)gg, (uint8_t)bb, a);
}

typedef struct shell_tones {
    flux_color toolbar;    /* slightly darker than bg                */
    flux_color sidebar;    /* lighter; visually a navigation column  */
    flux_color card;       /* card on top of the body                */
    flux_color status_bar; /* footer; darker than toolbar            */
    flux_color divider;    /* hairline between panels                */
} shell_tones;

static inline shell_tones shell_tones_from(const lens_theme *th) {
    /* Choose the shift direction: dark themes lighten, light themes
     * darken — both stay distinguishable. */
    uint8_t r, g, b, a;
    flux_color_unpack(th->color_bg, &r, &g, &b, &a);
    int sum = (int)r + g + b;
    int up = sum < 384 ? +1 : -1;
    /* On dark themes we mostly lighten layers up so panels read as
     * "raised"; the status bar drops below the body for footer feel.
     * The deltas are sized to be clearly distinguishable on a 14"
     * laptop panel at typical brightness. */
    shell_tones s = {
        .toolbar = shell_shift(th->color_bg, up * -10),
        .sidebar = shell_shift(th->color_bg, up * 14),
        .card = shell_shift(th->color_bg, up * 28),
        .status_bar = shell_shift(th->color_bg, up * -18),
        .divider = th->color_border,
    };
    return s;
}

#endif /* APP_SHELL_H */
