/* desktop_demo.c — desktop application layout with orthogonal primitives. */

#include "app_shell.h"
#include "ui_events.h"
#include <iris/app.h>
#include <iris/window.h>

#include <stdio.h>
#include <string.h>

enum { TABLE_ROWS = 50 };

typedef struct app {
    int selected_row;
    int sort_col;
    bool show_modal;
    int active_collection;
} app;

static ex_track g_tracks[EX_TRACK_MAX];
#define EVT(name) ex_handle(g_tracks, (name), lens_get_response(ui))

static const char *cell_name(int r) {
    static __thread char buf[64];
    snprintf(buf, sizeof buf, "asset-%04d", r);
    return buf;
}

static void build_menubar(lens *ui, app *a) {
    (void)a;
    if (lens_button(ui, &(lens_button_opts){.label = "File"}).clicked)
        lens_place_toggle(ui, "mb_file");
    if (lens_button(ui, &(lens_button_opts){.label = "Edit"}).clicked)
        lens_place_toggle(ui, "mb_edit");
    if (lens_button(ui, &(lens_button_opts){.label = "View"}).clicked)
        lens_place_toggle(ui, "mb_view");

    if (lens_place_begin(ui, &(lens_place_opts){.box = {.id = "mb_file"},
                                                .band = LENS_BAND_POPUP,
                                                .mode = LENS_PLACE_ANCHORED,
                                                .transient = true,
                                                .layout = {.pad = 6, .min_width = 120}})) {
        if (lens_button(ui, &(lens_button_opts){.label = "New"}).clicked) {
            printf("  MENU File > New\n");
            lens_place_close(ui, "mb_file");
        }
        if (lens_button(ui, &(lens_button_opts){.label = "Open…"}).clicked) {
            printf("  MENU File > Open\n");
            lens_place_close(ui, "mb_file");
        }
        lens_separator(ui, NULL);
        if (lens_button(ui, &(lens_button_opts){.label = "Quit"}).clicked) {
            printf("  MENU File > Quit\n");
            iris_window_close();
        }
        lens_place_end(ui);
    }
}

static void build_ui(lens *ui, const lens_input *in, void *user) {
    app *a = user;
    lens_theme th = lens_get_theme(ui);
    shell_tones tn = shell_tones_from(&th);

    for (uint32_t k = 0; k < in->key_count; k++)
        if (in->keys[k].pressed && in->keys[k].key == LENS_KEY_ESCAPE)
            iris_window_close();

    /* Menu bar */
    lens_row_begin(ui,
                   &(lens_layout_opts){.gap = 4, .pad = 4, .cross = LENS_CENTER, .bg = tn.toolbar});
    build_menubar(ui, a);
    lens_close(ui);

    /* Body: sidebar + content */
    lens_flex(ui, 1.0f);
    lens_row_begin(ui, &(lens_layout_opts){.gap = 0, .cross = LENS_STRETCH});

    /* Sidebar */
    lens_size(ui, 180, 0);
    lens_column_begin(
        ui, &(lens_layout_opts){.gap = 2, .pad = 10, .cross = LENS_STRETCH, .bg = tn.sidebar});
    lens_size(ui, 0, 28);
    lens_label(ui, &(lens_label_opts){.text = "Library", .size = 16.0f});
    for (int i = 0; i < 5; i++) {
        char label[32];
        snprintf(label, sizeof label, "Collection %d", i + 1);
        lens_size(ui, 0, 28);
        if (lens_selectable(ui, &(lens_selectable_opts){.label = label,
                                                        .selected = (a->active_collection == i)})
                .clicked) {
            a->active_collection = i;
            printf("  NAV %s\n", label);
        }
    }
    lens_close(ui);

    /* Content pane: virtualized scroll list */
    lens_flex(ui, 1.0f);
    lens_column_begin(
        ui, &(lens_layout_opts){.gap = 0, .pad = 0, .cross = LENS_STRETCH, .bg = tn.card});

    lens_size(ui, 0, 30);
    lens_row_begin(ui,
                   &(lens_layout_opts){.gap = 8, .pad = 6, .cross = LENS_CENTER, .bg = tn.toolbar});
    char hdr[64];
    snprintf(hdr, sizeof hdr, "%d assets · collection %d", TABLE_ROWS, a->active_collection + 1);
    lens_label(ui, &(lens_label_opts){.text = hdr});
    lens_flex(ui, 1.0f);
    lens_spacer(ui, 0);
    if (a->selected_row >= 0) {
        char sel[48];
        snprintf(sel, sizeof sel, "selected: %d", a->selected_row);
        lens_label(ui, &(lens_label_opts){.text = sel});
    }
    lens_close(ui);

    lens_flex(ui, 1.0f);
    lens_scroll_begin(ui, &(lens_scroll_opts){.box = {.id = "table_scroll"}});
    lens_column_begin(ui, &(lens_layout_opts){.pad = 4, .gap = 2, .cross = LENS_STRETCH});
    for (int i = 0; i < TABLE_ROWS; i++) {
        char row_lbl[64];
        snprintf(row_lbl, sizeof row_lbl, "%s   |   val: %d   |   2026-01-%02d", cell_name(i),
                 (i * 137) % 9999, (i % 28) + 1);
        if (lens_selectable(
                ui, &(lens_selectable_opts){.label = row_lbl, .selected = (a->selected_row == i)})
                .clicked) {
            a->selected_row = i;
            printf("  TABLE selected row %d\n", i);
        }
    }
    lens_close(ui);
    lens_scroll_end(ui);

    lens_close(ui); /* content pane */
    lens_close(ui); /* body row */

    /* Status bar */
    lens_row_begin(
        ui, &(lens_layout_opts){.gap = 10, .pad = 8, .cross = LENS_CENTER, .bg = tn.status_bar});
    lens_label(ui, &(lens_label_opts){.text = "iris desktop demo"});
    lens_flex(ui, 1.0f);
    lens_spacer(ui, 0);
    lens_size(ui, 110, 28);
    if (lens_button(ui, &(lens_button_opts){.label = "Open modal"}).clicked)
        lens_place_open(ui, "about_modal");
    EVT("Open modal");
    lens_close(ui);

    /* Modal */
    if (lens_place_is_open(ui, "about_modal")) {
        if (lens_place_begin(
                ui, &(lens_place_opts){
                        .box = {.id = "about_modal"},
                        .band = LENS_BAND_MODAL,
                        .mode = LENS_PLACE_CENTERED,
                        .transient = true,
                        .layout = {.pad = 20, .min_width = 340, .bg = tn.card, .radius = 8},
                    })) {
            lens_label(ui, &(lens_label_opts){.text = "About this demo", .size = 18.0f});
            lens_label(
                ui, &(lens_label_opts){.text = "Clean minimal orthogonal components in action."});
            lens_row_begin(ui, NULL);
            lens_flex(ui, 1.0f);
            lens_spacer(ui, 0);
            lens_size(ui, 90, 30);
            if (lens_button(ui, &(lens_button_opts){.label = "Close"}).clicked)
                lens_place_close(ui, "about_modal");
            lens_close(ui);
            lens_place_end(ui);
        }
    }
}

int main(void) {
    app a = {.selected_row = -1, .sort_col = 0, .show_modal = false, .active_collection = 0};

    printf("iris — desktop demo.\nEsc quits.\n\n");

    return iris_app_run(&(iris_app_config){
        .title = "iris — Desktop Demo",
        .width = 980,
        .height = 640,
        .dark = true,
        .log_raw = false,
        .build = build_ui,
        .user = &a,
    });
}
