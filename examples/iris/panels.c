/* panels.c — a "Workspace" file-browser mock on native Wayland.
 *
 *   ┌────────────────────────────────────────────────────────────┐
 *   │  toolbar: [New] [Open] [Save]  …  [Search bar]   [Settings]│
 *   ├──────────────────┬─────────────────────────────────────────┤
 *   │  sidebar         │  scrollable list of file rows           │
 *   │  • Inbox         │   [icon] [name........] [size] [time]   │
 *   │  • Projects      │   …                                     │
 *   │  • Archive       │   …                                     │
 *   │  • Trash         │                                         │
 *   ├──────────────────┴─────────────────────────────────────────┤
 *   │  status bar: [●] 24 items   [Progress slider]   [Cancel]   │
 *   └────────────────────────────────────────────────────────────┘
 *
 * Demonstrates: layout composition with panel backgrounds, scroll inside
 * a sized region, repeated row construction in a loop (each row has a
 * unique id), and event handling across both lens_response (hover / press
 * / focus) and the verb return value (click / value change).
 */

#include "app_shell.h"
#include "ui_events.h"
#include <iris/app.h>

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  App state                                                          */
/* ------------------------------------------------------------------ */

enum { N_FOLDERS = 4, N_FILES = 18 };

static const char *FOLDER_NAMES[N_FOLDERS] = {
    "Inbox",
    "Projects",
    "Archive",
    "Trash",
};

typedef struct file_row {
    const char *name;
    int size_kb;
    bool selected;
    bool starred;
} file_row;

typedef struct app {
    int active_folder;
    file_row files[N_FILES];
    int selected_idx;      /* -1 if none */
    float upload_progress; /* 0..1, slider */
    bool uploading;
    bool dark_theme;
} app;

static ex_track g_tracks[EX_TRACK_MAX];
#define EVT(name) ex_handle(g_tracks, (name), lens_get_response(ui))

/* ------------------------------------------------------------------ */
/*  Building blocks                                                    */
/* ------------------------------------------------------------------ */

/* A file row: [star] [name......] [size] [trash]
 * The whole row is a column-stretching child; the inner row arranges
 * its fixed-width cells. Returns the row's response so the caller can
 * react to clicks/hover on the whole row. */
static void file_row_widget(lens *ui, app *a, int idx, const shell_tones *tn) {
    file_row *f = &a->files[idx];
    bool active = (a->selected_idx == idx);

    lens_push_id_int(ui, idx);
    lens_size(ui, 0, 32);
    lens_row_ex(ui, (lens_layout_opts){.gap = 10,
                                       .pad = 8,
                                       .cross = LENS_CENTER,
                                       .bg = active ? tn->card : (flux_color){0},
                                       .radius = 6});

    /* star toggle (square button reads as the icon slot) */
    lens_size(ui, 18, 18);
    if (lens_button(ui, "##star")) {
        f->starred = !f->starred;
        printf("  STAR     [%2d] %s = %d\n", idx, f->name, f->starred);
    }
    EVT("file/star");

    /* name takes the rest of the row's main axis */
    lens_flex(ui, 1.0f);
    if (lens_button(ui, f->name)) {
        a->selected_idx = idx;
        printf("  SELECT   [%2d] %s\n", idx, f->name);
    }
    EVT("file/name");

    /* size cell — fixed-width slot so the column lines up */
    lens_size(ui, 64, 22);
    char sz[24];
    snprintf(sz, sizeof sz, "%d KB", f->size_kb);
    lens_label(ui, sz);

    /* trash */
    lens_size(ui, 22, 22);
    if (lens_button(ui, "##trash")) {
        printf("  DELETE   [%2d] %s\n", idx, f->name);
        f->name = "(deleted)";
    }
    EVT("file/trash");

    lens_close(ui);
    lens_pop_id(ui);
}

/* ------------------------------------------------------------------ */
/*  Build the whole frame                                              */
/* ------------------------------------------------------------------ */

static void build_ui(lens *ui, const lens_input *in, void *user) {
    app *a = user;
    lens_theme th = lens_get_theme(ui);
    shell_tones tn = shell_tones_from(&th);

    /* ── Toolbar ────────────────────────────────────────────── */
    lens_row_ex(ui,
                (lens_layout_opts){.gap = 6, .pad = 10, .cross = LENS_CENTER, .bg = tn.toolbar});

    lens_size(ui, 60, 28);
    if (lens_button(ui, "New"))
        printf("  ACTION   New\n");
    EVT("New");
    lens_size(ui, 60, 28);
    if (lens_button(ui, "Open"))
        printf("  ACTION   Open\n");
    EVT("Open");
    lens_size(ui, 60, 28);
    if (lens_button(ui, "Save"))
        printf("  ACTION   Save\n");
    EVT("Save");

    /* separator */
    lens_size(ui, 12, 0);
    lens_spacer(ui, 12);

    /* search "bar" — really an oversized button as a pill */
    lens_flex(ui, 1.0f);
    if (lens_button(ui, "Search...##search"))
        printf("  ACTION   focus search\n");
    EVT("Search");

    lens_size(ui, 80, 28);
    if (lens_checkbox(ui, "Dark", &a->dark_theme)) {
        lens_set_theme(ui, a->dark_theme ? lens_theme_dark() : lens_theme_default());
        printf("  TOGGLE   Theme = %s\n", a->dark_theme ? "dark" : "light");
    }
    EVT("Dark");

    lens_size(ui, 80, 28);
    if (lens_button(ui, "Settings"))
        printf("  ACTION   Settings\n");
    EVT("Settings");

    lens_close(ui); /* toolbar */

    /* ── Body: sidebar | content (grow to fill remaining height) ─ */
    lens_flex(ui, 1.0f);
    lens_row_ex(ui, (lens_layout_opts){.gap = 0, .cross = LENS_STRETCH});

    /* Sidebar */
    lens_size(ui, 180, 0);
    lens_column_ex(
        ui, (lens_layout_opts){.gap = 4, .pad = 10, .cross = LENS_STRETCH, .bg = tn.sidebar});
    lens_id active_folder_id = 0;
    for (int i = 0; i < N_FOLDERS; i++) {
        lens_size(ui, 0, 32);
        char id[40];
        snprintf(id, sizeof id, "%s##folder", FOLDER_NAMES[i]);
        if (lens_button(ui, id)) {
            a->active_folder = i;
            printf("  NAV      -> %s\n", FOLDER_NAMES[i]);
        }
        if (a->active_folder == i)
            active_folder_id = lens_get_response(ui).id;
        EVT(FOLDER_NAMES[i]);
    }
    if (active_folder_id)
        lens_set_focus(ui, active_folder_id);
    lens_flex(ui, 1.0f);
    lens_spacer(ui, 0);

    /* "New folder" sits at the bottom of the sidebar */
    lens_size(ui, 0, 32);
    if (lens_button(ui, "+ New folder"))
        printf("  ACTION   New folder\n");
    EVT("New folder");
    lens_close(ui);

    /* Content: a card holding the scrolling file list, padded so it
     * doesn't run flush against the sidebar. */
    lens_flex(ui, 1.0f);
    lens_column_ex(ui, (lens_layout_opts){.gap = 0, .pad = 16, .cross = LENS_STRETCH});

    /* Card */
    lens_flex(ui, 1.0f);
    lens_column_ex(ui, (lens_layout_opts){
                           .gap = 0, .pad = 0, .cross = LENS_STRETCH, .bg = tn.card, .radius = 8});

    /* Column header bar */
    lens_size(ui, 0, 28);
    lens_row_ex(ui,
                (lens_layout_opts){.gap = 10, .pad = 8, .cross = LENS_CENTER, .bg = tn.toolbar});
    lens_size(ui, 18, 14);
    lens_label(ui, "##h-star");
    lens_flex(ui, 1.0f);
    lens_label(ui, "Name");
    lens_size(ui, 64, 14);
    lens_label(ui, "Size");
    lens_size(ui, 22, 14);
    lens_label(ui, "##h-trash");
    lens_close(ui);

    /* Scrollable list */
    lens_scroll_begin(ui, "filelist");
    lens_column_ex(ui, (lens_layout_opts){.pad = 8, .gap = 0, .cross = LENS_STRETCH});
    for (int i = 0; i < N_FILES; i++)
        file_row_widget(ui, a, i, &tn);
    lens_close(ui);
    lens_scroll_end(ui);

    lens_close(ui); /* card */

    lens_close(ui); /* content padding */

    lens_close(ui); /* body row */

    /* ── Status bar ─────────────────────────────────────────── */
    lens_row_ex(
        ui, (lens_layout_opts){.gap = 10, .pad = 10, .cross = LENS_CENTER, .bg = tn.status_bar});
    /* Status dot */
    lens_size(ui, 10, 10);
    lens_button(ui, "##statusdot");

    char buf[64];
    snprintf(buf, sizeof buf, "%d items   selected: %d", N_FILES, a->selected_idx);
    lens_label(ui, buf);

    lens_flex(ui, 1.0f);
    lens_spacer(ui, 0);

    /* Upload progress slider — labelled by position, not text */
    lens_size(ui, 220, 22);
    if (lens_slider(ui, "##upload", &a->upload_progress, 0.0f, 1.0f)) {
        a->uploading = (a->upload_progress > 0.001f && a->upload_progress < 0.999f);
        printf("  PROGRESS %.0f%%\n", a->upload_progress * 100.0f);
    }
    EVT("upload");

    lens_size(ui, 80, 28);
    if (lens_button(ui, "Cancel")) {
        a->upload_progress = 0.0f;
        a->uploading = false;
        printf("  ACTION   Cancel upload\n");
    }
    EVT("Cancel upload");
    lens_close(ui);
}

/* ------------------------------------------------------------------ */
/*  Entry point                                                        */
/* ------------------------------------------------------------------ */

int main(void) {
    static const struct {
        const char *name;
        int kb;
    } seed[N_FILES] = {
        {"README.md", 4},
        {"design-notes.txt", 12},
        {"build.log", 128},
        {"screenshot.png", 842},
        {"report-final.pdf", 916},
        {"data.csv", 340},
        {"shader.frag", 3},
        {"shader.vert", 2},
        {"main.c", 18},
        {"main.o", 32},
        {"config.toml", 1},
        {"icon-set.zip", 1240},
        {"minutes.md", 7},
        {"draft.docx", 54},
        {"old-design.png", 1280},
        {"old-design@2x.png", 4096},
        {"notes.org", 22},
        {"TODO.md", 5},
    };
    app a = {
        .active_folder = 1, /* Projects */
        .selected_idx = 0,
        .upload_progress = 0.35f,
        .uploading = true,
        .dark_theme = true,
    };
    for (int i = 0; i < N_FILES; i++) {
        a.files[i].name = seed[i].name;
        a.files[i].size_kb = seed[i].kb;
        a.files[i].starred = (i % 5 == 0);
        a.files[i].selected = (i == a.selected_idx);
    }

    printf("iris — Workspace mock. Native Wayland, HiDPI-aware, real\n"
           "text via FreeType+HarfBuzz (font found through fontconfig).\n"
           "Override the font family with $FLUX_TEXT_FONT. Esc quits.\n\n");

    return iris_app_run(&(iris_app_config){
        .title = "iris — Workspace",
        .width = 1040,
        .height = 700,
        .dark = true,
        .log_raw = false,
        .build = build_ui,
        .user = &a,
    });
}
