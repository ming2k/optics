/*
 * hello_app.c — canonical minimal Optics GUI application (ADR-0171).
 *
 * Demonstrates:
 *   - Launching a native window with optics_app_run;
 *   - Immediate-mode layout and interactive state with optics_ui;
 *   - Keyboard shortcut handling (Esc to quit) and dark/light switching.
 */

#include <optics/app.h>
#include <optics/ui.h>

#include <stdio.h>

typedef struct hello_state {
    int clicks;
    bool dark;
} hello_state;

static void build(optics_ui *ui, const optics_ui_input *in, void *user) {
    hello_state *state = user;

    for (uint32_t k = 0; k < in->key_count; k++) {
        if (in->keys[k].pressed && in->keys[k].key == OPTICS_UI_KEY_ESCAPE) {
            optics_app_quit();
            return;
        }
    }

    optics_ui_column_begin(
        ui, &(optics_ui_layout_opts){.pad = 32, .gap = 16, .cross = OPTICS_UI_START});

    optics_ui_label(ui, &(optics_ui_label_opts){
                            .text = "Hello, Optics",
                            .size = 24.0f,
                            .weight = 650.0f,
                        });

    char summary[96];
    snprintf(summary, sizeof summary, "Button clicked %d time%s. Press Esc to close.",
             state->clicks, state->clicks == 1 ? "" : "s");
    optics_ui_label(ui, &(optics_ui_label_opts){.text = summary, .size = 14.0f});

    optics_ui_row_begin(ui, &(optics_ui_layout_opts){.gap = 10, .cross = OPTICS_UI_CENTER});
    if (optics_ui_button(
            ui, &(optics_ui_button_opts){.label = "Increment", .variant = OPTICS_UI_BUTTON_PRIMARY})
            .clicked) {
        state->clicks++;
    }
    if (optics_ui_button(ui,
                         &(optics_ui_button_opts){
                             .label = state->dark ? "Light theme" : "Dark theme",
                             .variant = OPTICS_UI_BUTTON_SUBTLE,
                         })
            .clicked) {
        state->dark = !state->dark;
        optics_ui_set_theme(ui, state->dark ? optics_ui_theme_dark() : optics_ui_theme_default());
    }
    optics_ui_close(ui);

    optics_ui_close(ui);
}

int main(void) {
    hello_state state = {.dark = true};
    return optics_app_run(&(optics_app_opts){
        .title = "Optics — Hello App",
        .app_id = "org.optics.getting-started.hello-app",
        .width = 460,
        .height = 220,
        .dark = state.dark,
        .build = build,
        .user = &state,
    });
}
