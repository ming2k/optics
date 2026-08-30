/*
 * state_demo.c — Demonstrates how to use lens_node_state for retained state.
 *
 * Build: meson setup build -Dexamples=true && ./build/examples/lens/state_demo
 */

#include <lens/lens.h>
#include <stdio.h>
#include <string.h>

/* A custom toggle widget built from lens primitives, holding its own state. */
static void custom_toggle(lens *ui, const char *label) {
    /* 1. Generate a stable ID for this widget based on the label */
    lens_id id = lens_current_id(ui, label);

    /* 2. Push an interactive button */
    lens_response r = lens_button(ui, &(lens_button_opts){.label = label});

    /* 3. Ask the retained store for 1 byte of state for this ID. */
    lens_node *n = lens_find(ui, id);
    if (n) {
        bool *is_on = (bool *)lens_node_state(n, sizeof(bool));

        if (r.clicked) {
            *is_on = !(*is_on); /* Toggle the state */
        }

        printf("Widget '%s' (id:%016llx) is currently %s (clicked this frame: %s)\n", label,
               (unsigned long long)id, *is_on ? "ON" : "OFF", r.clicked ? "yes" : "no");
    }
}

int main(void) {
    lens *ui = NULL;
    if (lens_create(&(lens_desc){.theme = lens_theme_default()}, &ui) != FLUX_OK) {
        return 1;
    }

    /* Simulate 4 frames */
    for (int frame = 0; frame < 4; frame++) {
        lens_input in = {.display_size = {400, 300}, .dt_seconds = 1.0f / 60.0f};

        /* Frame 1: press mouse */
        if (frame == 1) {
            in.cursor = (flux_point){20, 20};
            in.mouse_down[LENS_MOUSE_LEFT] = true;
            in.mouse_pressed[LENS_MOUSE_LEFT] = true;
        }
        /* Frame 2: release mouse */
        else if (frame == 2) {
            in.cursor = (flux_point){20, 20};
            in.mouse_released[LENS_MOUSE_LEFT] = true;
        }

        lens_begin(ui, &in);
        printf("--- Frame %d ---\n", frame);
        lens_column_begin(ui, NULL);
        custom_toggle(ui, "Auto-Save");
        custom_toggle(ui, "Dark Mode");
        lens_close(ui);
        lens_end(ui);
    }

    lens_destroy(ui);
    return 0;
}
