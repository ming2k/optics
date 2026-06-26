/* hello_ui.c — a "Preferences" dialog mock on native Wayland.
 *
 * The window is laid out as a real app shell — toolbar, sidebar, content
 * card, status bar — with text rendered via the FreeType/HarfBuzz backend:
 *
 *   ┌────────────────────────────────────────────────────────────┐
 *   │  toolbar (title + window-style buttons)                    │
 *   ├──────────────────┬─────────────────────────────────────────┤
 *   │                  │                                         │
 *   │  sidebar         │  content card                           │
 *   │   • General      │   (controls for the active sidebar tab) │
 *   │   • Appearance   │                                         │
 *   │   • Editor       │                                         │
 *   │   • Privacy      │                                         │
 *   │                  │                                         │
 *   ├──────────────────┴─────────────────────────────────────────┤
 *   │  status bar / footer (Cancel — Apply)                      │
 *   └────────────────────────────────────────────────────────────┘
 *
 * Events: each widget verb returns its primary edge (click / value
 * change), handled inline; lens_get_response() is diffed into
 * hover/press/focus events by ui_events.h.
 */

#include "app_shell.h"
#include "ui_events.h"
#include <iris/app.h>

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  App state                                                          */
/* ------------------------------------------------------------------ */

typedef enum { TAB_GENERAL = 0, TAB_APPEARANCE, TAB_EDITOR, TAB_PRIVACY, TAB_COUNT } tab_id;

static const char *TAB_LABELS[TAB_COUNT] = {
    "General",
    "Appearance",
    "Editor",
    "Privacy",
};

typedef struct app {
    tab_id active_tab;
    bool dark_theme;

    /* General */
    bool auto_save;
    bool send_telemetry;
    float idle_timeout;

    /* Appearance */
    float ui_scale_hint; /* layout-only knob; real scale is from compositor */
    float accent_hue;
    bool show_minimap;

    /* Editor */
    float font_size;
    float tab_width;
    bool wrap_lines;
    bool show_whitespace;
    bool format_on_save;

    /* Privacy */
    bool share_crash_reports;
    bool collect_usage;
    bool third_party_sync;

    bool dirty; /* anything changed since last Apply */
} app;

static ex_track g_tracks[EX_TRACK_MAX];
#define EVT(name) ex_handle(g_tracks, (name), lens_get_response(ui))

/* ------------------------------------------------------------------ */
/*  Building blocks                                                    */
/* ------------------------------------------------------------------ */

/* A sidebar item: a full-width fixed-height button. The active row is
 * marked with the focus border (set after the loop); returns true on
 * click. */
static bool sidebar_item(lens *ui, const char *label, bool active) {
    lens_size(ui, 0, 34);
    /* The "##nav-N" suffix gives the row a stable id distinct from the
     * label, since two examples may share label text. */
    char buf[64];
    snprintf(buf, sizeof buf, "%s##nav", label);
    (void)active; /* visual highlight comes from accent/active state */
    return lens_button(ui, buf);
}

/* A labeled control row — a fixed-height row that holds [spacer, control]
 * so the control sits to the right; with text disabled the label slot is
 * blank space, but the rhythm still reads as a settings form. */
static void form_row_begin(lens *ui, const char *id) {
    lens_push_id(ui, id);
    lens_row_ex(ui, (lens_layout_opts){.gap = 12, .cross = LENS_CENTER});
    lens_size(ui, 140, 28);
    lens_label(ui, id); /* the "label" column */
}
static void form_row_end(lens *ui) {
    lens_close(ui);
    lens_pop_id(ui);
}

/* ------------------------------------------------------------------ */
/*  Per-tab content                                                    */
/* ------------------------------------------------------------------ */

static void content_general(lens *ui, app *a) {
    form_row_begin(ui, "Auto-save");
    if (lens_checkbox(ui, "##autosave", &a->auto_save)) {
        a->dirty = true;
        printf("  TOGGLE   Auto-save = %s\n", a->auto_save ? "on" : "off");
    }
    EVT("Auto-save");
    form_row_end(ui);

    form_row_begin(ui, "Telemetry");
    if (lens_checkbox(ui, "##telemetry", &a->send_telemetry)) {
        a->dirty = true;
        printf("  TOGGLE   Telemetry = %s\n", a->send_telemetry ? "on" : "off");
    }
    EVT("Telemetry");
    form_row_end(ui);

    form_row_begin(ui, "Idle timeout");
    lens_flex(ui, 1.0f);
    if (lens_slider(ui, "##idle", &a->idle_timeout, 0.0f, 60.0f)) {
        a->dirty = true;
        printf("  VALUE    Idle timeout = %.0f min\n", a->idle_timeout);
    }
    EVT("Idle timeout");
    form_row_end(ui);
}

static void content_appearance(lens *ui, app *a) {
    form_row_begin(ui, "Theme");
    if (lens_checkbox(ui, "Dark", &a->dark_theme)) {
        lens_set_theme(ui, a->dark_theme ? lens_theme_dark() : lens_theme_default());
        a->dirty = true;
        printf("  TOGGLE   Theme = %s\n", a->dark_theme ? "dark" : "light");
    }
    EVT("Theme");
    form_row_end(ui);

    form_row_begin(ui, "UI density");
    lens_flex(ui, 1.0f);
    if (lens_slider(ui, "##density", &a->ui_scale_hint, 0.85f, 1.40f)) {
        a->dirty = true;
        printf("  VALUE    UI density = %.2f\n", a->ui_scale_hint);
    }
    EVT("UI density");
    form_row_end(ui);

    form_row_begin(ui, "Accent hue");
    lens_flex(ui, 1.0f);
    if (lens_slider(ui, "##hue", &a->accent_hue, 0.0f, 360.0f)) {
        a->dirty = true;
        printf("  VALUE    Accent hue = %.0f\n", a->accent_hue);
    }
    EVT("Accent hue");
    form_row_end(ui);

    form_row_begin(ui, "Minimap");
    if (lens_checkbox(ui, "##minimap", &a->show_minimap)) {
        a->dirty = true;
        printf("  TOGGLE   Minimap = %s\n", a->show_minimap ? "on" : "off");
    }
    EVT("Minimap");
    form_row_end(ui);
}

static void content_editor(lens *ui, app *a) {
    form_row_begin(ui, "Font size");
    lens_flex(ui, 1.0f);
    if (lens_slider(ui, "##font", &a->font_size, 9.0f, 28.0f)) {
        a->dirty = true;
        printf("  VALUE    Font size = %.1f\n", a->font_size);
    }
    EVT("Font size");
    form_row_end(ui);

    form_row_begin(ui, "Tab width");
    lens_flex(ui, 1.0f);
    if (lens_slider(ui, "##tabw", &a->tab_width, 1.0f, 8.0f)) {
        a->dirty = true;
        printf("  VALUE    Tab width = %.0f\n", a->tab_width);
    }
    EVT("Tab width");
    form_row_end(ui);

    form_row_begin(ui, "Wrap lines");
    if (lens_checkbox(ui, "##wrap", &a->wrap_lines)) {
        a->dirty = true;
        printf("  TOGGLE   Wrap = %d\n", a->wrap_lines);
    }
    EVT("Wrap lines");
    form_row_end(ui);

    form_row_begin(ui, "Show whitespace");
    if (lens_checkbox(ui, "##ws", &a->show_whitespace)) {
        a->dirty = true;
        printf("  TOGGLE   Whitespace = %d\n", a->show_whitespace);
    }
    EVT("Show whitespace");
    form_row_end(ui);

    form_row_begin(ui, "Format on save");
    if (lens_checkbox(ui, "##fmt", &a->format_on_save)) {
        a->dirty = true;
        printf("  TOGGLE   Format on save = %d\n", a->format_on_save);
    }
    EVT("Format on save");
    form_row_end(ui);
}

static void content_privacy(lens *ui, app *a) {
    form_row_begin(ui, "Crash reports");
    if (lens_checkbox(ui, "##crash", &a->share_crash_reports)) {
        a->dirty = true;
        printf("  TOGGLE   Crash reports = %d\n", a->share_crash_reports);
    }
    EVT("Crash reports");
    form_row_end(ui);

    form_row_begin(ui, "Usage stats");
    if (lens_checkbox(ui, "##usage", &a->collect_usage)) {
        a->dirty = true;
        printf("  TOGGLE   Usage stats = %d\n", a->collect_usage);
    }
    EVT("Usage stats");
    form_row_end(ui);

    form_row_begin(ui, "3rd-party sync");
    if (lens_checkbox(ui, "##3p", &a->third_party_sync)) {
        a->dirty = true;
        printf("  TOGGLE   3rd-party sync = %d\n", a->third_party_sync);
    }
    EVT("3rd-party sync");
    form_row_end(ui);
}

/* ------------------------------------------------------------------ */
/*  Build the whole frame                                              */
/* ------------------------------------------------------------------ */

static void build_ui(lens *ui, const lens_input *in, void *user) {
    app *a = user;
    lens_theme th = lens_get_theme(ui); /* tracks Dark toggles */
    shell_tones tn = shell_tones_from(&th);

    /* ── Toolbar ────────────────────────────────────────────── */
    lens_row_ex(ui, (lens_layout_opts){
                        .gap = 8, .pad = 10, .cross = LENS_CENTER, .bg = tn.toolbar, .radius = 0});
    lens_title(ui, "Preferences");
    lens_flex(ui, 1.0f);
    lens_spacer(ui, 0);
    lens_size(ui, 28, 22);
    if (lens_button(ui, "##min"))
        printf("  WIN      minimise\n");
    EVT("min");
    lens_size(ui, 28, 22);
    if (lens_button(ui, "##max"))
        printf("  WIN      maximise\n");
    EVT("max");
    lens_size(ui, 28, 22);
    if (lens_button(ui, "##close"))
        printf("  WIN      close\n");
    EVT("close");
    lens_close(ui);

    /* ── Body: sidebar | content (grow to fill remaining height) ─ */
    lens_flex(ui, 1.0f);
    lens_row_ex(ui, (lens_layout_opts){.gap = 0, .cross = LENS_STRETCH});

    /* Sidebar */
    lens_size(ui, 180, 0);
    lens_column_ex(ui,
                   (lens_layout_opts){
                       .gap = 4, .pad = 10, .cross = LENS_STRETCH, .bg = tn.sidebar, .radius = 0});
    lens_id active_tab_id = 0;
    for (int i = 0; i < TAB_COUNT; i++) {
        if (sidebar_item(ui, TAB_LABELS[i], a->active_tab == (tab_id)i)) {
            a->active_tab = (tab_id)i;
            printf("  NAV      -> %s\n", TAB_LABELS[i]);
        }
        if (a->active_tab == (tab_id)i)
            active_tab_id = lens_get_response(ui).id;
        EVT(TAB_LABELS[i]);
    }
    /* The focus border doubles as the "selected tab" indicator
     * — a button with no glyph would otherwise look identical to
     * its neighbours. */
    if (active_tab_id)
        lens_set_focus(ui, active_tab_id);

    lens_flex(ui, 1.0f);
    lens_spacer(ui, 0);
    /* Sign-out at the bottom of the sidebar */
    lens_size(ui, 0, 34);
    if (lens_button(ui, "Sign out"))
        printf("  ACTION   Sign out\n");
    EVT("Sign out");
    lens_close(ui);

    /* Content card */
    lens_flex(ui, 1.0f);
    lens_column_ex(ui, (lens_layout_opts){.gap = 14, .pad = 24, .cross = LENS_STRETCH});
    /* Card surface */
    lens_column_ex(ui,
                   (lens_layout_opts){
                       .gap = 10, .pad = 18, .cross = LENS_STRETCH, .bg = tn.card, .radius = 8});
    switch (a->active_tab) {
    case TAB_GENERAL:
        content_general(ui, a);
        break;
    case TAB_APPEARANCE:
        content_appearance(ui, a);
        break;
    case TAB_EDITOR:
        content_editor(ui, a);
        break;
    case TAB_PRIVACY:
        content_privacy(ui, a);
        break;
    default:
        break;
    }
    lens_close(ui);
    lens_flex(ui, 1.0f);
    lens_spacer(ui, 0); /* push status to bottom */
    lens_close(ui);

    lens_close(ui); /* body row */

    /* ── Status bar / footer ─────────────────────────────────── */
    lens_row_ex(ui,
                (lens_layout_opts){
                    .gap = 8, .pad = 10, .cross = LENS_CENTER, .bg = tn.status_bar, .radius = 0});
    /* dirty indicator: a small accent square when there are unsaved changes */
    lens_size(ui, 10, 10);
    if (a->dirty)
        lens_button(ui, "##dirty");
    else
        lens_label(ui, "##clean");
    lens_label(ui, a->dirty ? "Unsaved changes" : "Up to date");
    lens_flex(ui, 1.0f);
    lens_spacer(ui, 0);
    lens_size(ui, 90, 28);
    if (lens_button(ui, "Cancel"))
        printf("  ACTION   Cancel\n");
    EVT("Cancel");
    lens_size(ui, 90, 28);
    if (lens_button(ui, "Apply")) {
        a->dirty = false;
        printf("  ACTION   Apply\n");
    }
    EVT("Apply");
    lens_close(ui);
}

/* ------------------------------------------------------------------ */
/*  Entry point                                                        */
/* ------------------------------------------------------------------ */

int main(void) {
    app a = {
        .active_tab = TAB_GENERAL,
        .dark_theme = true,
        .auto_save = true,
        .send_telemetry = false,
        .idle_timeout = 10.0f,
        .ui_scale_hint = 1.0f,
        .accent_hue = 220.0f,
        .show_minimap = true,
        .font_size = 13.0f,
        .tab_width = 4.0f,
        .wrap_lines = false,
        .show_whitespace = false,
        .format_on_save = true,
        .share_crash_reports = true,
        .collect_usage = false,
        .third_party_sync = false,
        .dirty = false,
    };

    printf("iris — Preferences dialog. Native Wayland, HiDPI-aware,\n"
           "real text via FreeType+HarfBuzz (font found through fontconfig).\n"
           "Override the font family with $FLUX_TEXT_FONT. Esc quits.\n\n");

    return iris_app_run(&(iris_app_config){
        .title = "iris — Preferences",
        .width = 880,
        .height = 600,
        .dark = true,
        .log_raw = false,
        .build = build_ui,
        .user = &a,
    });
}
