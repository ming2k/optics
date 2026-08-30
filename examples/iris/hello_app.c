/* hello_app.c — a "Preferences" dialog mock on native Wayland. */

#include "app_shell.h"
#include "ui_events.h"
#include <iris/app.h>
#include <iris/window.h>

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

static bool sidebar_item(lens *ui, const char *label, bool active) {
    lens_size(ui, 0, 34);
    return lens_selectable(ui, &(lens_selectable_opts){.label = label, .selected = active}).clicked;
}

static void form_row_begin(lens *ui, const char *id) {
    lens_row_begin(ui, &(lens_layout_opts){.gap = 12, .cross = LENS_CENTER});
    lens_size(ui, 140, 28);
    lens_label(ui, &(lens_label_opts){.text = id});
}
static void form_row_end(lens *ui) {
    lens_close(ui);
}

/* ------------------------------------------------------------------ */
/*  Per-tab content                                                    */
/* ------------------------------------------------------------------ */

static void content_general(lens *ui, app *a) {
    form_row_begin(ui, "Auto-save");
    if (lens_checkbox(ui, &(lens_checkbox_opts){.label = "##autosave", .value = &a->auto_save})
            .changed) {
        a->dirty = true;
        printf("  TOGGLE   Auto-save = %s\n", a->auto_save ? "on" : "off");
    }
    EVT("Auto-save");
    form_row_end(ui);

    form_row_begin(ui, "Telemetry");
    if (lens_checkbox(ui,
                      &(lens_checkbox_opts){.label = "##telemetry", .value = &a->send_telemetry})
            .changed) {
        a->dirty = true;
        printf("  TOGGLE   Telemetry = %s\n", a->send_telemetry ? "on" : "off");
    }
    EVT("Telemetry");
    form_row_end(ui);

    form_row_begin(ui, "Idle timeout");
    lens_flex(ui, 1.0f);
    if (lens_slider(ui,
                    &(lens_slider_opts){
                        .label = "##idle", .value = &a->idle_timeout, .min = 0.0f, .max = 60.0f})
            .changed) {
        a->dirty = true;
        printf("  VALUE    Idle timeout = %.0f min\n", a->idle_timeout);
    }
    EVT("Idle timeout");
    form_row_end(ui);
}

static void content_appearance(lens *ui, app *a) {
    form_row_begin(ui, "Theme");
    if (lens_checkbox(ui, &(lens_checkbox_opts){.label = "Dark", .value = &a->dark_theme})
            .changed) {
        lens_set_theme(ui, a->dark_theme ? lens_theme_dark() : lens_theme_default());
        a->dirty = true;
        printf("  TOGGLE   Theme = %s\n", a->dark_theme ? "dark" : "light");
    }
    EVT("Theme");
    form_row_end(ui);

    form_row_begin(ui, "UI density");
    lens_flex(ui, 1.0f);
    if (lens_slider(ui, &(lens_slider_opts){.label = "##density",
                                            .value = &a->ui_scale_hint,
                                            .min = 0.85f,
                                            .max = 1.40f})
            .changed) {
        a->dirty = true;
        printf("  VALUE    UI density = %.2f\n", a->ui_scale_hint);
    }
    EVT("UI density");
    form_row_end(ui);

    form_row_begin(ui, "Accent hue");
    lens_flex(ui, 1.0f);
    if (lens_slider(ui,
                    &(lens_slider_opts){
                        .label = "##hue", .value = &a->accent_hue, .min = 0.0f, .max = 360.0f})
            .changed) {
        a->dirty = true;
        printf("  VALUE    Accent hue = %.0f\n", a->accent_hue);
    }
    EVT("Accent hue");
    form_row_end(ui);

    form_row_begin(ui, "Minimap");
    if (lens_checkbox(ui, &(lens_checkbox_opts){.label = "##minimap", .value = &a->show_minimap})
            .changed) {
        a->dirty = true;
        printf("  TOGGLE   Minimap = %s\n", a->show_minimap ? "on" : "off");
    }
    EVT("Minimap");
    form_row_end(ui);
}

static void content_editor(lens *ui, app *a) {
    form_row_begin(ui, "Font size");
    lens_flex(ui, 1.0f);
    if (lens_slider(ui,
                    &(lens_slider_opts){
                        .label = "##font", .value = &a->font_size, .min = 9.0f, .max = 28.0f})
            .changed) {
        a->dirty = true;
        printf("  VALUE    Font size = %.1f\n", a->font_size);
    }
    EVT("Font size");
    form_row_end(ui);

    form_row_begin(ui, "Tab width");
    lens_flex(ui, 1.0f);
    if (lens_slider(ui,
                    &(lens_slider_opts){
                        .label = "##tabw", .value = &a->tab_width, .min = 1.0f, .max = 8.0f})
            .changed) {
        a->dirty = true;
        printf("  VALUE    Tab width = %.0f\n", a->tab_width);
    }
    EVT("Tab width");
    form_row_end(ui);

    form_row_begin(ui, "Wrap lines");
    if (lens_checkbox(ui, &(lens_checkbox_opts){.label = "##wrap", .value = &a->wrap_lines})
            .changed) {
        a->dirty = true;
        printf("  TOGGLE   Wrap = %d\n", a->wrap_lines);
    }
    EVT("Wrap lines");
    form_row_end(ui);

    form_row_begin(ui, "Show whitespace");
    if (lens_checkbox(ui, &(lens_checkbox_opts){.label = "##ws", .value = &a->show_whitespace})
            .changed) {
        a->dirty = true;
        printf("  TOGGLE   Whitespace = %d\n", a->show_whitespace);
    }
    EVT("Show whitespace");
    form_row_end(ui);

    form_row_begin(ui, "Format on save");
    if (lens_checkbox(ui, &(lens_checkbox_opts){.label = "##fmt", .value = &a->format_on_save})
            .changed) {
        a->dirty = true;
        printf("  TOGGLE   Format on save = %d\n", a->format_on_save);
    }
    EVT("Format on save");
    form_row_end(ui);
}

static void content_privacy(lens *ui, app *a) {
    form_row_begin(ui, "Crash reports");
    if (lens_checkbox(ui,
                      &(lens_checkbox_opts){.label = "##crash", .value = &a->share_crash_reports})
            .changed) {
        a->dirty = true;
        printf("  TOGGLE   Crash reports = %d\n", a->share_crash_reports);
    }
    EVT("Crash reports");
    form_row_end(ui);

    form_row_begin(ui, "Usage stats");
    if (lens_checkbox(ui, &(lens_checkbox_opts){.label = "##usage", .value = &a->collect_usage})
            .changed) {
        a->dirty = true;
        printf("  TOGGLE   Usage stats = %d\n", a->collect_usage);
    }
    EVT("Usage stats");
    form_row_end(ui);

    form_row_begin(ui, "3rd-party sync");
    if (lens_checkbox(ui, &(lens_checkbox_opts){.label = "##3p", .value = &a->third_party_sync})
            .changed) {
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
    lens_theme th = lens_get_theme(ui);
    shell_tones tn = shell_tones_from(&th);

    /* Esc quits (the startup line says so). */
    for (uint32_t k = 0; k < in->key_count; k++)
        if (in->keys[k].pressed && in->keys[k].key == LENS_KEY_ESCAPE)
            iris_window_close();

    /* ── Toolbar ────────────────────────────────────────────── */
    lens_row_begin(ui,
                   &(lens_layout_opts){
                       .gap = 8, .pad = 10, .cross = LENS_CENTER, .bg = tn.toolbar, .radius = 0});
    lens_label(ui, &(lens_label_opts){.text = "Preferences", .size = 18.0f});
    lens_flex(ui, 1.0f);
    lens_spacer(ui, 0);
    lens_size(ui, 28, 22);
    if (lens_button(ui, &(lens_button_opts){.label = "##min"}).clicked)
        iris_window_minimize();
    EVT("min");
    lens_size(ui, 28, 22);
    if (lens_button(ui, &(lens_button_opts){.label = "##max"}).clicked)
        iris_window_maximize();
    EVT("max");
    lens_size(ui, 28, 22);
    if (lens_button(ui, &(lens_button_opts){.label = "##close"}).clicked)
        iris_window_close();
    EVT("close");
    lens_close(ui);

    /* ── Body: sidebar | content (grow to fill remaining height) ─ */
    lens_flex(ui, 1.0f);
    lens_row_begin(ui, &(lens_layout_opts){.gap = 0, .cross = LENS_STRETCH});

    /* Sidebar */
    lens_size(ui, 180, 0);
    lens_column_begin(
        ui, &(lens_layout_opts){
                .gap = 4, .pad = 10, .cross = LENS_STRETCH, .bg = tn.sidebar, .radius = 0});
    for (int i = 0; i < TAB_COUNT; i++) {
        if (sidebar_item(ui, TAB_LABELS[i], a->active_tab == (tab_id)i)) {
            a->active_tab = (tab_id)i;
            printf("  NAV      -> %s\n", TAB_LABELS[i]);
        }
        EVT(TAB_LABELS[i]);
    }

    lens_flex(ui, 1.0f);
    lens_spacer(ui, 0);
    /* Sign-out at the bottom of the sidebar */
    lens_size(ui, 0, 34);
    if (lens_button(ui, &(lens_button_opts){.label = "Sign out"}).clicked)
        printf("  ACTION   Sign out\n");
    EVT("Sign out");
    lens_close(ui);

    /* Content card */
    lens_flex(ui, 1.0f);
    lens_column_begin(ui, &(lens_layout_opts){.gap = 14, .pad = 24, .cross = LENS_STRETCH});
    /* Card surface */
    lens_column_begin(ui,
                      &(lens_layout_opts){
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
    lens_spacer(ui, 0);
    lens_close(ui);

    lens_close(ui); /* body row */

    /* ── Status bar / footer ─────────────────────────────────── */
    lens_row_begin(
        ui, &(lens_layout_opts){
                .gap = 8, .pad = 10, .cross = LENS_CENTER, .bg = tn.status_bar, .radius = 0});
    lens_label(ui, &(lens_label_opts){.text = a->dirty ? "Unsaved changes" : "Up to date"});
    lens_flex(ui, 1.0f);
    lens_spacer(ui, 0);
    lens_size(ui, 90, 28);
    if (lens_button(ui, &(lens_button_opts){.label = "Cancel"}).clicked)
        printf("  ACTION   Cancel\n");
    EVT("Cancel");
    lens_size(ui, 90, 28);
    if (lens_button(ui, &(lens_button_opts){.label = "Apply", .variant = LENS_BUTTON_PRIMARY})
            .clicked) {
        a->dirty = false;
        printf("  ACTION   Apply\n");
    }
    EVT("Apply");
    lens_close(ui);
}

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

    printf("iris — Preferences dialog.\n\n");

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
