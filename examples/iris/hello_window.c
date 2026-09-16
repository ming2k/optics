/*
 * hello_window.c — canonical starter for an iris windowed application.
 *
 * Demonstrates the minimal application setup:
 *   - Native window creation and lifecycle via iris_app_run;
 *   - Immediate-mode UI layout with lens widgets (column, label, button);
 *   - Event handling and graceful shutdown.
 */

#include <iris/app.h>
#include <iris/window.h>
#include <lens/lens.h>

#include <stdio.h>

static void build(lens *ui, const lens_input *in, void *user) {
    (void)user;

    /* Esc key closes the window */
    for (uint32_t k = 0; k < in->key_count; k++) {
        if (in->keys[k].pressed && in->keys[k].key == LENS_KEY_ESCAPE)
            iris_window_close();
    }

    lens_column_begin(ui, &(lens_layout_opts){.pad = 28, .gap = 16, .cross = LENS_START});
    lens_label(ui, &(lens_label_opts){.text = "Hello from Optics & Iris!", .size = 18.0f});

    if (lens_button(ui, &(lens_button_opts){.label = "Click me", .variant = LENS_BUTTON_PRIMARY})
            .clicked) {
        printf("[hello_window] button clicked!\n");
    }

    lens_close(ui);
}

int main(void) {
    printf("iris hello_window — canonical starter application (Esc to quit)\n\n");
    return iris_app_run(&(iris_app_config){
        .title = "iris — hello_window",
        .width = 400,
        .height = 200,
        .dark = true,
        .build = build,
    });
}
