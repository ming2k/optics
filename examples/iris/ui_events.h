/* ui_events.h — turn lens_response snapshots into discrete events.
 *
 * lens has no per-widget callbacks (ADR-0006): each widget verb returns
 * its primary edge (a button's .clicked, a slider/checkbox's .changed),
 * and lens_get_response() exposes the full lens_response for the widget just
 * emitted. The level state in that response (hovered/pressed/focused) is
 * not edge-triggered, so this helper diffs it against last frame and
 * prints enter/leave style events — the application's own event handling,
 * built on top of the snapshot.
 *
 * Usage:
 *     static ex_track g_tracks[EX_TRACK_MAX];
 *     #define EVT(name) ex_handle(g_tracks, (name), lens_get_response(ui))
 *     ...
 *     if (lens_button(ui, "Save")) save();   // click via return value
 *     EVT("Save");                          // hover/press/focus via response
 */
#ifndef UI_EVENTS_H
#define UI_EVENTS_H

#include <lens/lens.h>
#include <stdio.h>
#include <string.h>

#define EX_TRACK_MAX 64

typedef struct ex_track {
    char name[28];
    bool used, hovered, pressed, focused;
} ex_track;

static inline void ex_handle(ex_track *tab, const char *name, lens_response r) {
    ex_track *t = NULL, *slot = NULL;
    for (int i = 0; i < EX_TRACK_MAX; i++) {
        if (tab[i].used) {
            if (strcmp(tab[i].name, name) == 0) {
                t = &tab[i];
                break;
            }
        } else if (!slot) {
            slot = &tab[i];
        }
    }
    if (!t && slot) {
        t = slot;
        t->used = true;
        snprintf(t->name, sizeof t->name, "%s", name);
    }
    if (!t)
        return;

    if (r.hovered && !t->hovered)
        printf("  hover  >  %s\n", name);
    if (!r.hovered && t->hovered)
        printf("  hover  <  %s\n", name);
    if (r.pressed && !t->pressed)
        printf("  press     %s\n", name);
    if (!r.pressed && t->pressed)
        printf("  release   %s\n", name);
    if (r.focused && !t->focused)
        printf("  focus  +  %s\n", name);
    if (!r.focused && t->focused)
        printf("  focus  -  %s\n", name);

    t->hovered = r.hovered;
    t->pressed = r.pressed;
    t->focused = r.focused;
}

#endif /* UI_EVENTS_H */
