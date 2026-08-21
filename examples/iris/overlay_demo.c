/* overlay_demo.c — Dropdown menus and modal dialog with placed popups.
 *
 * Demonstrates:
 *   - lens_place_open / lens_place_begin / lens_place_end (ADR-0060)
 *   - click-outside and Escape dismissal of transient nodes
 *   - band occlusion (base widgets under a popup are not interactive)
 *   - a fake modal backdrop using a full-screen semi-transparent rect
 *
 * Layout:
 *   ┌─────────────────────────────────────────┐
 *   │  [File ▼] [Edit ▼]        [Dark] [Modal]│
 *   ├─────────────────────────────────────────┤
 *   │                                         │
 *   │  Content                                │
 *   │                                         │
 *   └─────────────────────────────────────────┘
 */

#include "app_shell.h"
#include "ui_events.h"
#include <iris/app.h>
#include <iris/window.h>
#include <lens/lens.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  App state                                                          */
/* ------------------------------------------------------------------ */

typedef struct app {
    bool dark_theme;
    char text_buf[64];

    /* Anchor rects for dropdown buttons (prev_rect, updated each frame). */
    flux_rect file_btn_rect;
    flux_rect edit_btn_rect;
} app;

static ex_track g_tracks[EX_TRACK_MAX];
#define EVT(name) ex_handle(g_tracks, (name), lens_get_response(ui))

/* ------------------------------------------------------------------ */
/*  Menu items (thin separator + button rows)                          */
/* ------------------------------------------------------------------ */

static void menu_separator(lens *ui, shell_tones *tn) {
    lens_size(ui, 0, 1);
    lens_row_ex(ui, (lens_layout_opts){.pad = 0, .bg = tn->divider});
    lens_spacer(ui, 0);
    lens_close(ui);
}

/* ------------------------------------------------------------------ */
/*  Per-frame build                                                    */
/* ------------------------------------------------------------------ */

static void build_ui(lens *ui, const lens_input *in, void *user) {
    app *a = user;
    lens_theme th = lens_get_theme(ui);
    shell_tones tn = shell_tones_from(&th);

    /* Esc quits the application (the startup line says so); lens also
     * dismisses the top popup on the same key. */
    for (uint32_t k = 0; k < in->key_count; k++)
        if (in->keys[k].pressed && in->keys[k].key == LENS_KEY_ESCAPE)
            iris_window_close();

    /* ── Base tree ─────────────────────────────────────────────── */

    lens_column_ex(ui, (lens_layout_opts){.pad = 0, .gap = 0, .cross = LENS_STRETCH});

    /* Toolbar */
    lens_row_ex(ui, (lens_layout_opts){
                        .gap = 8, .pad = 10, .cross = LENS_CENTER, .bg = tn.toolbar, .radius = 0});

    if (lens_button(ui, "File")) {
        lens_place_open(ui, "file_menu");
        printf("  MENU     File opened\n");
    }
    a->file_btn_rect = lens_get_response(ui).rect;
    EVT("File");

    if (lens_button(ui, "Edit")) {
        lens_place_open(ui, "edit_menu");
        printf("  MENU     Edit opened\n");
    }
    a->edit_btn_rect = lens_get_response(ui).rect;
    EVT("Edit");

    lens_flex(ui, 1.0f);
    lens_spacer(ui, 0);

    lens_size(ui, 0, 28);
    if (lens_checkbox(ui, "Dark", &a->dark_theme))
        lens_set_theme(ui, a->dark_theme ? lens_theme_dark() : lens_theme_default());

    lens_size(ui, 100, 28);
    if (lens_button(ui, "Modal…")) {
        lens_place_open(ui, "modal");
        printf("  MODAL    Opened\n");
    }
    EVT("Modal");
    lens_close(ui);

    /* Body */
    lens_flex(ui, 1.0f);
    lens_column_ex(
        ui, (lens_layout_opts){.pad = 24, .gap = 14, .cross = LENS_STRETCH, .bg = th.color_bg});

    lens_heading(ui, "Placement & Z-Band Demo", 1);
    lens_label(ui, "• File / Edit open anchored popups (LENS_PLACE_ANCHORED).");
    lens_label(ui, "• Click outside any transient popup to close it.");
    lens_label(ui, "• Esc closes the top transient popup.");
    lens_label(ui, "• Modal draws a fake backdrop over the base tree.");

    lens_row(ui);
    lens_size(ui, 80, 0);
    lens_label(ui, "Name:");
    lens_flex(ui, 1.0f);
    if (lens_textfield(ui, "##name", a->text_buf, sizeof a->text_buf))
        printf("  TEXT     name = %s\n", a->text_buf);
    lens_close(ui);
    lens_close(ui);

    lens_close(ui); /* main column */

    /* ── File dropdown popup ───────────────────────────────────── */

    if (lens_place_is_open(ui, "file_menu")) {
        if (lens_place_begin(ui, "file_menu",
                             (lens_place_opts){
                                 .band = LENS_BAND_POPUP,
                                 .mode = LENS_PLACE_ANCHORED,
                                 .rect = a->file_btn_rect,
                                 .transient = true,
                                 .layout = {.pad = 6, .min_width = 140, .bg = tn.card, .radius = 6},
                             })) {
            if (lens_button(ui, "New")) {
                printf("  FILE     New\n");
                lens_place_close(ui, "file_menu");
            }
            if (lens_button(ui, "Open")) {
                printf("  FILE     Open\n");
                lens_place_close(ui, "file_menu");
            }
            if (lens_button(ui, "Save")) {
                printf("  FILE     Save\n");
                lens_place_close(ui, "file_menu");
            }
            menu_separator(ui, &tn);
            if (lens_button(ui, "Exit")) {
                printf("  FILE     Exit\n");
                lens_place_close(ui, "file_menu");
            }
            lens_place_end(ui);
        }
    }

    /* ── Edit dropdown popup ───────────────────────────────────── */

    if (lens_place_is_open(ui, "edit_menu")) {
        if (lens_place_begin(ui, "edit_menu",
                             (lens_place_opts){
                                 .band = LENS_BAND_POPUP,
                                 .mode = LENS_PLACE_ANCHORED,
                                 .rect = a->edit_btn_rect,
                                 .transient = true,
                                 .layout = {.pad = 6, .min_width = 140, .bg = tn.card, .radius = 6},
                             })) {
            if (lens_button(ui, "Cut")) {
                printf("  EDIT     Cut\n");
                lens_place_close(ui, "edit_menu");
            }
            if (lens_button(ui, "Copy")) {
                printf("  EDIT     Copy\n");
                lens_place_close(ui, "edit_menu");
            }
            if (lens_button(ui, "Paste")) {
                printf("  EDIT     Paste\n");
                lens_place_close(ui, "edit_menu");
            }
            lens_place_end(ui);
        }
    }

    /* ── Modal popup (fake backdrop) ───────────────────────────── */

    if (lens_place_is_open(ui, "modal")) {
        /* A modal dim must sit ABOVE the base tree, so it goes into the
         * POPUP band (not BACKDROP, which paints below the base tree) just
         * before the dialog content: same band, later registration = on top. */
        flux_color dim =
            a->dark_theme ? flux_color_rgba(0, 0, 0, 0xAA) : flux_color_rgba(255, 255, 255, 0xAA);
        if (lens_place_begin(
                ui, "modal##bd",
                (lens_place_opts){
                    .band = LENS_BAND_POPUP,
                    .mode = LENS_PLACE_EXACT,
                    .rect = {0, 0, in ? in->display_size.x : 0, in ? in->display_size.y : 0},
                    .layout = {.bg = dim},
                })) {
            lens_place_end(ui);
        }

        if (lens_place_begin(
                ui, "modal",
                (lens_place_opts){
                    .band = LENS_BAND_POPUP,
                    .mode = LENS_PLACE_CENTERED,
                    .transient = true,
                    .layout = {.pad = 20, .min_width = 280, .bg = tn.card, .radius = 10},
                })) {
            lens_label(ui, "Modal Dialog");
            lens_label(ui, "Popups can stack. Escape or click outside to close.");
            lens_row(ui);
            lens_flex(ui, 1.0f);
            lens_spacer(ui, 0);
            if (lens_button(ui, "OK")) {
                printf("  MODAL    OK\n");
                lens_place_close(ui, "modal");
            }
            if (lens_button(ui, "Cancel")) {
                printf("  MODAL    Cancel\n");
                lens_place_close(ui, "modal");
            }
            lens_close(ui);
            lens_place_end(ui);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Entry point                                                        */
/* ------------------------------------------------------------------ */

int main(void) {
    app a = {
        .dark_theme = true,
        .text_buf = {0},
        .file_btn_rect = {0},
        .edit_btn_rect = {0},
    };

    printf("iris — Placement & dropdown demo. Native Wayland, HiDPI-aware.\n"
           "Click File or Edit for dropdowns. Click Modal for a dialog.\n"
           "Esc quits the application; it also dismisses the top popup.\n\n");

    return iris_app_run(&(iris_app_config){
        .title = "iris — Placement Demo",
        .width = 720,
        .height = 480,
        .dark = true,
        .log_raw = false,
        .build = build_ui,
        .user = &a,
    });
}
