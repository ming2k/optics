/* overlay_demo.c — Dropdown menus and modal dialog with placed popups. */

#include "app_shell.h"
#include "ui_events.h"
#include <iris/app.h>
#include <iris/window.h>
#include <lens/lens.h>
#include <stdio.h>
#include <string.h>

typedef struct app {
    bool dark_theme;
    char text_buf[64];
    flux_rect file_btn_rect;
    flux_rect edit_btn_rect;
} app;

static ex_track g_tracks[EX_TRACK_MAX];
#define EVT(name) ex_handle(g_tracks, (name), lens_get_response(ui))

static void menu_separator(lens *ui, shell_tones *tn) {
    lens_size(ui, 0, 1);
    lens_row_begin(ui, &(lens_layout_opts){.pad = 0, .bg = tn->divider});
    lens_spacer(ui, 0);
    lens_close(ui);
}

static void build_ui(lens *ui, const lens_input *in, void *user) {
    app *a = user;
    lens_theme th = lens_get_theme(ui);
    shell_tones tn = shell_tones_from(&th);

    for (uint32_t k = 0; k < in->key_count; k++)
        if (in->keys[k].pressed && in->keys[k].key == LENS_KEY_ESCAPE)
            iris_window_close();

    lens_column_begin(ui, &(lens_layout_opts){.pad = 0, .gap = 0, .cross = LENS_STRETCH});

    /* Toolbar */
    lens_row_begin(ui,
                   &(lens_layout_opts){
                       .gap = 8, .pad = 10, .cross = LENS_CENTER, .bg = tn.toolbar, .radius = 0});

    if (lens_button(ui, &(lens_button_opts){.label = "File"}).clicked) {
        lens_place_open(ui, "file_menu");
        printf("  MENU     File opened\n");
    }
    a->file_btn_rect = lens_get_response(ui).rect;
    EVT("File");

    if (lens_button(ui, &(lens_button_opts){.label = "Edit"}).clicked) {
        lens_place_open(ui, "edit_menu");
        printf("  MENU     Edit opened\n");
    }
    a->edit_btn_rect = lens_get_response(ui).rect;
    EVT("Edit");

    lens_flex(ui, 1.0f);
    lens_spacer(ui, 0);

    lens_size(ui, 0, 28);
    if (lens_checkbox(ui, &(lens_checkbox_opts){.label = "Dark", .value = &a->dark_theme}).changed)
        lens_set_theme(ui, a->dark_theme ? lens_theme_dark() : lens_theme_default());

    lens_size(ui, 100, 28);
    if (lens_button(ui, &(lens_button_opts){.label = "Modal…"}).clicked) {
        lens_place_open(ui, "modal");
        printf("  MODAL    Opened\n");
    }
    EVT("Modal");
    lens_close(ui);

    /* Body */
    lens_flex(ui, 1.0f);
    lens_column_begin(
        ui, &(lens_layout_opts){.pad = 24, .gap = 14, .cross = LENS_STRETCH, .bg = th.color_bg});

    lens_label(ui, &(lens_label_opts){.text = "Placement & Z-Band Demo", .size = 20.0f});
    lens_label(ui, &(lens_label_opts){
                       .text = "• File / Edit open anchored popups (LENS_PLACE_ANCHORED)."});
    lens_label(ui, &(lens_label_opts){.text = "• Click outside any transient popup to close it."});
    lens_label(ui, &(lens_label_opts){.text = "• Esc closes the top transient popup."});
    lens_label(ui, &(lens_label_opts){.text = "• Modal draws a fake backdrop over the base tree."});

    lens_row_begin(ui, NULL);
    lens_size(ui, 80, 0);
    lens_label(ui, &(lens_label_opts){.text = "Name:"});
    lens_flex(ui, 1.0f);
    if (lens_textedit(ui, &(lens_textedit_opts){.box = {.id = "name"},
                                                .buf = a->text_buf,
                                                .cap = sizeof a->text_buf})
            .changed)
        printf("  TEXT     name = %s\n", a->text_buf);
    lens_close(ui);
    lens_close(ui);

    lens_close(ui); /* main column */

    /* ── File dropdown popup ───────────────────────────────────── */

    if (lens_place_is_open(ui, "file_menu")) {
        if (lens_place_begin(ui,
                             &(lens_place_opts){
                                 .box = {.id = "file_menu"},
                                 .band = LENS_BAND_POPUP,
                                 .mode = LENS_PLACE_ANCHORED,
                                 .rect = a->file_btn_rect,
                                 .transient = true,
                                 .layout = {.pad = 6, .min_width = 140, .bg = tn.card, .radius = 6},
                             })) {
            if (lens_button(ui, &(lens_button_opts){.label = "New"}).clicked) {
                printf("  FILE     New\n");
                lens_place_close(ui, "file_menu");
            }
            if (lens_button(ui, &(lens_button_opts){.label = "Open"}).clicked) {
                printf("  FILE     Open\n");
                lens_place_close(ui, "file_menu");
            }
            if (lens_button(ui, &(lens_button_opts){.label = "Save"}).clicked) {
                printf("  FILE     Save\n");
                lens_place_close(ui, "file_menu");
            }
            menu_separator(ui, &tn);
            if (lens_button(ui, &(lens_button_opts){.label = "Exit"}).clicked) {
                printf("  FILE     Exit\n");
                lens_place_close(ui, "file_menu");
            }
            lens_place_end(ui);
        }
    }

    /* ── Edit dropdown popup ───────────────────────────────────── */

    if (lens_place_is_open(ui, "edit_menu")) {
        if (lens_place_begin(ui,
                             &(lens_place_opts){
                                 .box = {.id = "edit_menu"},
                                 .band = LENS_BAND_POPUP,
                                 .mode = LENS_PLACE_ANCHORED,
                                 .rect = a->edit_btn_rect,
                                 .transient = true,
                                 .layout = {.pad = 6, .min_width = 140, .bg = tn.card, .radius = 6},
                             })) {
            if (lens_button(ui, &(lens_button_opts){.label = "Cut"}).clicked) {
                printf("  EDIT     Cut\n");
                lens_place_close(ui, "edit_menu");
            }
            if (lens_button(ui, &(lens_button_opts){.label = "Copy"}).clicked) {
                printf("  EDIT     Copy\n");
                lens_place_close(ui, "edit_menu");
            }
            if (lens_button(ui, &(lens_button_opts){.label = "Paste"}).clicked) {
                printf("  EDIT     Paste\n");
                lens_place_close(ui, "edit_menu");
            }
            lens_place_end(ui);
        }
    }

    /* ── Modal popup (fake backdrop) ───────────────────────────── */

    if (lens_place_is_open(ui, "modal")) {
        flux_color dim =
            a->dark_theme ? flux_color_rgba(0, 0, 0, 0xAA) : flux_color_rgba(255, 255, 255, 0xAA);
        if (lens_place_begin(
                ui, &(lens_place_opts){
                        .box = {.id = "modal_bd"},
                        .band = LENS_BAND_POPUP,
                        .mode = LENS_PLACE_EXACT,
                        .rect = {0, 0, in ? in->display_size.x : 0, in ? in->display_size.y : 0},
                        .layout = {.bg = dim},
                    })) {
            lens_place_end(ui);
        }

        if (lens_place_begin(
                ui, &(lens_place_opts){
                        .box = {.id = "modal"},
                        .band = LENS_BAND_POPUP,
                        .mode = LENS_PLACE_CENTERED,
                        .transient = true,
                        .layout = {.pad = 20, .min_width = 280, .bg = tn.card, .radius = 10},
                    })) {
            lens_label(ui, &(lens_label_opts){.text = "Modal Dialog", .size = 18.0f});
            lens_label(ui, &(lens_label_opts){
                               .text = "Popups can stack. Escape or click outside to close."});
            lens_row_begin(ui, NULL);
            lens_flex(ui, 1.0f);
            lens_spacer(ui, 0);
            if (lens_button(ui, &(lens_button_opts){.label = "OK"}).clicked) {
                printf("  MODAL    OK\n");
                lens_place_close(ui, "modal");
            }
            if (lens_button(ui, &(lens_button_opts){.label = "Cancel"}).clicked) {
                printf("  MODAL    Cancel\n");
                lens_place_close(ui, "modal");
            }
            lens_close(ui);
            lens_place_end(ui);
        }
    }
}

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
