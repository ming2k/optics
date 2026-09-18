/*
 * main.c — Entry point and view coordinator for the Overlays scenario.
 *
 * Demonstrates:
 *   1. Anchored popup placement (dropdown menus)
 *   2. Centered modal dialogs with backdrop scrim
 *   3. Material-aware theme switching (Light / Dark mode)
 *   4. Zero-privilege public API consumption and headless --smoke verification
 */

#include "overlays.h"

void draw_toolbar(lens *ui, overlay_app_state *state, const shell_tones *tones) {
    lens_row_begin(
        ui, &(lens_layout_opts){
                .gap = 8, .pad = 10, .cross = LENS_CENTER, .bg = tones->toolbar, .radius = 0});

    /* File dropdown trigger */
    if (lens_button(ui, &(lens_button_opts){.label = "File"}).clicked) {
        lens_place_open(ui, "file_menu");
        printf("  [TOOLBAR] File opened\n");
    }
    state->file_btn_rect = lens_get_response(ui).rect;

    /* Edit dropdown trigger */
    if (lens_button(ui, &(lens_button_opts){.label = "Edit"}).clicked) {
        lens_place_open(ui, "edit_menu");
        printf("  [TOOLBAR] Edit opened\n");
    }
    state->edit_btn_rect = lens_get_response(ui).rect;

    lens_flex(ui, 1.0f);
    lens_spacer(ui, 0);

    /* Theme Switcher */
    lens_size(ui, 0, 28);
    if (lens_checkbox(ui,
                      &(lens_checkbox_opts){
                          .label = "Dark Mode",
                          .value = &state->dark_theme,
                          .appearance = LENS_CHECKBOX_SWITCH,
                      })
            .changed) {
        lens_set_theme(ui, state->dark_theme ? lens_theme_dark() : lens_theme_default());
        printf("  [THEME] Switched to %s\n", state->dark_theme ? "Dark (Smoke)" : "Light (Pearl)");
    }

    /* Modal dialog trigger */
    lens_size(ui, 110, 28);
    if (lens_button(ui, &(lens_button_opts){.label = "Modal Dialog…"}).clicked) {
        lens_place_open(ui, "modal");
        printf("  [TOOLBAR] Modal opened\n");
    }

    lens_close(ui); /* toolbar row */
}

void draw_main_content(lens *ui, overlay_app_state *state, const lens_theme *theme) {
    lens_flex(ui, 1.0f);
    lens_column_begin(ui, &(lens_layout_opts){
                              .pad = 24, .gap = 14, .cross = LENS_STRETCH, .bg = theme->color_bg});

    lens_label(ui, &(lens_label_opts){.text = "Overlays & Modals Scenario", .size = 22.0f});
    lens_label(
        ui,
        &(lens_label_opts){
            .text = "• Click 'File' or 'Edit' on the toolbar to open anchored menus.\n"
                    "• Click 'Modal Dialog…' to open a centered modal with backdrop scrim.\n"
                    "• Toggle 'Dark Mode' to verify live theme & material adaptation.\n"
                    "• Click anywhere outside a transient popup or press Escape to dismiss it."});

    /* Text input row */
    lens_row_begin(ui, &(lens_layout_opts){.gap = 12, .cross = LENS_CENTER});
    lens_size(ui, 80, 0);
    lens_label(ui, &(lens_label_opts){.text = "Username:"});
    lens_flex(ui, 1.0f);
    if (lens_textedit(ui,
                      &(lens_textedit_opts){
                          .box = {.id = "input_user"},
                          .buf = state->user_name,
                          .cap = sizeof state->user_name,
                      })
            .changed) {
        printf("  [INPUT] user = %s\n", state->user_name);
    }
    lens_close(ui);

    lens_close(ui); /* main column */
}

static void build_ui(lens *ui, const lens_input *in, void *user) {
    overlay_app_state *state = user;
    lens_theme theme = lens_get_theme(ui);
    shell_tones tones = shell_tones_from(&theme);

    /* CI Smoke test exit */
    if (state->smoke_mode) {
        state->smoke_frames++;
        if (state->smoke_frames >= 3) {
            iris_window_close();
            return;
        }
    }

    /* Escape hotkey closes top transient or the window */
    for (uint32_t k = 0; k < in->key_count; k++) {
        if (in->keys[k].pressed && in->keys[k].key == LENS_KEY_ESCAPE) {
            if (!lens_place_is_open(ui, "modal") && !lens_place_is_open(ui, "file_menu") &&
                !lens_place_is_open(ui, "edit_menu")) {
                iris_window_close();
            }
        }
    }

    lens_column_begin(ui, &(lens_layout_opts){.pad = 0, .gap = 0, .cross = LENS_STRETCH});
    draw_toolbar(ui, state, &tones);
    draw_main_content(ui, state, &theme);
    lens_close(ui);

    /* Transient Popups and Modals */
    draw_anchored_menus(ui, state, &tones);
    draw_modal_dialog(ui, in, state, &tones);
}

int main(int argc, char **argv) {
    overlay_app_state state = {
        .dark_theme = true,
        .user_name = "Optics Developer",
        .file_btn_rect = {0},
        .edit_btn_rect = {0},
        .smoke_mode = false,
        .smoke_frames = 0,
    };

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--smoke") == 0)
            state.smoke_mode = true;
    }

    if (!state.smoke_mode) {
        printf("Optics — Overlays & Popups Scenario (Multi-File Architecture)\n"
               "Demonstrating anchored dropdowns, modal scrims, and material themes.\n"
               "Esc closes open popups or exits.\n\n");
    }

    return iris_app_run(&(iris_app_config){
        .title = "Optics — Overlays Scenario",
        .width = 720,
        .height = 480,
        .dark = state.dark_theme,
        .log_raw = false,
        .build = build_ui,
        .user = &state,
    });
}
