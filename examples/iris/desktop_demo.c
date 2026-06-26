/* desktop_demo.c — all four ADR-0016..0019 widgets in one Wayland window.
 *
 *   ┌────────────────────────────────────────────────────────────┐
 *   │  menubar: File  Edit  View                                  │
 *   ├──────────────────┬─────────────────────────────────────────┤
 *   │  sidebar         │  table (virtualized, 5000 rows)          │
 *   │  • list          │   Name            Value     Modified     │
 *   │  • split handle  │   ...                                   │
 *   ├──────────────────┴─────────────────────────────────────────┤
 *   │  status bar · [Open modal]                                  │
 *   └────────────────────────────────────────────────────────────┘
 *
 * Verifies the four new widgets (modal, menu bar/items/submenus, context
 * menu, resizable split, virtualized table) compose in a real iris frame
 * loop on Wayland. Right-click the table for a context menu; "Open modal"
 * raises a dismissable dialog with a focus trap.
 */

#include "app_shell.h"
#include "ui_events.h"
#include <iris/app.h>

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  App state                                                          */
/* ------------------------------------------------------------------ */

enum { TABLE_ROWS = 5000 };

typedef struct app {
    int selected_row; /* -1 = none */
    int sort_col;     /* 0..2 */
    bool show_modal;
    float split_ratio; /* persisted sidebar width */
} app;

static ex_track g_tracks[EX_TRACK_MAX];
#define EVT(name) ex_handle(g_tracks, (name), lens_get_response(ui))

/* Synthetic table data: deterministic per row so no storage is needed. */
static const char *cell_name(int r) {
    static __thread char buf[64];
    snprintf(buf, sizeof buf, "asset-%04d", r);
    return buf;
}
static const char *cell_value(int r) {
    static __thread char buf[32];
    snprintf(buf, sizeof buf, "%d", r * 137 % 9999);
    return buf;
}
static const char *cell_modified(int r) {
    static __thread char buf[32];
    snprintf(buf, sizeof buf, "2026-01-%02d", (r % 28) + 1);
    return buf;
}

static const char *table_cell(void *user, int row, int col) {
    (void)user;
    switch (col) {
    case 0:
        return cell_name(row);
    case 1:
        return cell_value(row);
    case 2:
        return cell_modified(row);
    }
    return "";
}

static const lens_table_column TABLE_COLS[3] = {
    {.title = "Name", .width = 160, .align = LENS_START},
    {.title = "Value", .width = 0, .align = LENS_END},
    {.title = "Modified", .width = 120, .align = LENS_START},
};

/* ------------------------------------------------------------------ */
/*  Build                                                              */
/* ------------------------------------------------------------------ */

/* Menu bar: exercises menubar, menus, items, separators, submenus. */
static void build_menubar(lens *ui, app *a) {
    if (lens_menubar_begin(ui, "mb")) {
        if (lens_menu_begin(ui, "File")) {
            if (lens_menu_item(ui, "New", "Ctrl-N"))
                printf("  MENU  File > New\n");
            if (lens_menu_item(ui, "Open…", "Ctrl-O"))
                printf("  MENU  File > Open\n");
            lens_menu_separator(ui);
            if (lens_submenu_begin(ui, "Recent")) {
                if (lens_menu_item(ui, "asset-0001", NULL))
                    printf("  MENU  Recent > asset-0001\n");
                if (lens_menu_item(ui, "asset-0002", NULL))
                    printf("  MENU  Recent > asset-0002\n");
                lens_submenu_end(ui);
            }
            lens_menu_separator(ui);
            if (lens_menu_item_disabled(ui, "Save", "Ctrl-S")) {
            }
            if (lens_menu_item(ui, "Quit", "Ctrl-Q"))
                printf("  MENU  File > Quit\n");
            lens_menu_end(ui);
        }
        if (lens_menu_begin(ui, "Edit")) {
            if (lens_menu_item_flags(ui, "Cut", "Ctrl-X", LENS_MENU_DISABLED)) {
            }
            if (lens_menu_item_flags(ui, "Copy", "Ctrl-C", LENS_MENU_CHECKED))
                printf("  MENU  Edit > Copy\n");
            if (lens_menu_item(ui, "Paste", "Ctrl-V"))
                printf("  MENU  Edit > Paste\n");
            lens_menu_end(ui);
        }
        if (lens_menu_begin(ui, "View")) {
            if (lens_menu_item_flags(ui, "Sort by Name", NULL,
                                     a->sort_col == 0 ? LENS_MENU_RADIO | LENS_MENU_CHECKED
                                                      : LENS_MENU_RADIO))
                a->sort_col = 0;
            if (lens_menu_item_flags(ui, "Sort by Value", NULL,
                                     a->sort_col == 1 ? LENS_MENU_RADIO | LENS_MENU_CHECKED
                                                      : LENS_MENU_RADIO))
                a->sort_col = 1;
            lens_menu_end(ui);
        }
        lens_menubar_end(ui);
    }
}

static void build_ui(lens *ui, const lens_input *in, void *user) {
    app *a = user;
    lens_theme th = lens_get_theme(ui);
    shell_tones tn = shell_tones_from(&th);

    /* ── Menu bar ───────────────────────────────────────────── */
    lens_row_ex(ui, (lens_layout_opts){.gap = 0, .pad = 4, .cross = LENS_CENTER, .bg = tn.toolbar});
    build_menubar(ui, a);
    lens_close(ui);

    /* ── Body: resizable sidebar | content ──────────────────── */
    lens_flex(ui, 1.0f);
    lens_split_begin(
        ui, "main", LENS_SPLIT_VERTICAL,
        &(lens_split_opts){
            .ratio = a->split_ratio, .min_first = 140, .min_second = 240, .thickness = 6});

    /* Sidebar pane */
    lens_split_pane(ui);
    lens_column_ex(
        ui, (lens_layout_opts){.gap = 2, .pad = 10, .cross = LENS_STRETCH, .bg = tn.sidebar});
    lens_size(ui, 0, 28);
    lens_title(ui, "Library");
    for (int i = 0; i < 5; i++) {
        char label[32];
        snprintf(label, sizeof label, "Collection %d", i + 1);
        lens_size(ui, 0, 28);
        if (lens_selectable(ui, label, i == 0))
            printf("  NAV   %s\n", label);
    }
    lens_close(ui);

    /* Content pane: the virtualized table */
    lens_split_pane(ui);
    lens_column_ex(ui,
                   (lens_layout_opts){.gap = 0, .pad = 0, .cross = LENS_STRETCH, .bg = tn.card});

    /* Context menu: right-click the table area to open it. */
    lens_size(ui, 0, 30);
    lens_row_ex(ui, (lens_layout_opts){.gap = 8, .pad = 6, .cross = LENS_CENTER, .bg = tn.toolbar});
    char hdr[64];
    snprintf(hdr, sizeof hdr, "%d assets · sort: col %d", TABLE_ROWS, a->sort_col);
    lens_label(ui, hdr);
    lens_flex(ui, 1.0f);
    lens_spacer(ui, 0);
    if (a->selected_row >= 0) {
        char sel[48];
        snprintf(sel, sizeof sel, "selected: %d", a->selected_row);
        lens_label(ui, sel);
    }
    lens_close(ui);

    /* The table: fills the rest of the pane. A right-click on its
     * response opens a context menu at the cursor. */
    lens_flex(ui, 1.0f);
    lens_table_result tr =
        lens_table(ui, "assets", TABLE_COLS, 3, TABLE_ROWS, table_cell, a,
                   (lens_table_opts){
                       .row_height = 26, .show_header = true, .selectable = true, .zebra = true});
    lens_response tr_resp = lens_get_response(ui);
    if (tr_resp.right_clicked)
        lens_context_menu_open(ui, "row", tr_resp.rect);

    if (lens_context_menu_begin(ui, "row")) {
        if (lens_menu_item(ui, "Open", NULL)) {
            printf("  CTX   Open row %d\n", tr.selected);
            a->show_modal = true;
        }
        if (lens_menu_item(ui, "Duplicate", NULL))
            printf("  CTX   Duplicate row %d\n", tr.selected);
        lens_menu_separator(ui);
        if (lens_menu_item_disabled(ui, "Delete", "Del")) {
        }
        lens_context_menu_end(ui);
    }

    if (tr.selection_changed) {
        a->selected_row = tr.selected;
        printf("  TABLE selected row %d\n", tr.selected);
    }

    lens_close(ui); /* content pane */

    lens_split_end(ui);
    a->split_ratio = lens_split_ratio(ui, "main"); /* persist */

    /* ── Status bar ─────────────────────────────────────────── */
    lens_row_ex(ui,
                (lens_layout_opts){.gap = 10, .pad = 8, .cross = LENS_CENTER, .bg = tn.status_bar});
    lens_size(ui, 10, 10);
    lens_button(ui, "##dot");
    lens_label(ui, "iris desktop demo");
    lens_flex(ui, 1.0f);
    lens_spacer(ui, 0);
    lens_size(ui, 110, 28);
    if (lens_button(ui, "Open modal"))
        a->show_modal = true;
    EVT("Open modal");
    lens_close(ui);

    /* ── Modal dialog ───────────────────────────────────────── */
    if (a->show_modal)
        lens_modal_open(ui, "about");

    if (lens_modal_begin(ui, "about",
                         (lens_modal_opts){.title = "About this demo",
                                           .backdrop = 0x90000000,
                                           .min_width = 340,
                                           .dismissable = true})) {
        lens_label(ui, "This window composes every ADR-0016..0019 widget:");
        lens_label(ui, "menu bar · submenu · context menu ·");
        lens_label(ui, "resizable split · virtualized table · modal.");
        lens_row(ui);
        lens_flex(ui, 1.0f);
        lens_spacer(ui, 0);
        lens_size(ui, 90, 30);
        if (lens_button(ui, "Close"))
            a->show_modal = false;
        lens_close(ui);
        lens_modal_end(ui);
    }
}

/* ------------------------------------------------------------------ */
/*  Entry                                                              */
/* ------------------------------------------------------------------ */

int main(void) {
    app a = {.selected_row = -1, .sort_col = 0, .show_modal = false, .split_ratio = 0.22f};

    printf("iris — desktop demo. ADR-0016..0019 widgets on native Wayland.\n"
           "  • Menu bar + submenus + context menu (right-click the table)\n"
           "  • Resizable split (drag the divider)\n"
           "  • Virtualized table (%d rows, only visible ones build)\n"
           "  • Modal dialog (Open modal, or Escape to dismiss)\n"
           "Esc quits.\n\n",
           TABLE_ROWS);

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
