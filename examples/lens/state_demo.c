/*
 * state_demo.c — Demonstrates how to use lens_node_state for retained state.
 *
 * lens is an immediate-mode API over a retained-mode core. You can ask the
 * core to store a few bytes of state for a widget across frames by calling
 * lens_node_state() with the widget's ID. This is useful for building
 * custom interactive widgets (like a toggle switch or a collapsible section)
 * where you don't want the host application to have to store the local
 * visual state.
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

    /* 2. Push an interactive box (we'll just use a button for the hit-test) */
    lens_button_opts opts = {.label = label};
    lens_response r = lens_button_ex(ui, opts);

    /* 3. Ask the retained store for 1 byte of state for this ID.
     *    On the first frame this ID is seen, it will be zeroed out.
     *    On subsequent frames, it returns the same pointer. */
    lens_node *n = lens_find(ui, id);
    if (n) {
        bool *is_on = (bool *)lens_node_state(n, sizeof(bool));

        if (r.clicked) {
            *is_on = !(*is_on); /* Toggle the state */
        }

        /* We can't easily draw custom shapes in pure headless lens without
         * tapping into lensi_ internals, but we can print the retained state! */
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
        lens_column(ui);
        custom_toggle(ui, "Auto-Save");
        custom_toggle(ui, "Dark Mode");
        lens_close(ui);
        lens_end(ui);
    }

    lens_destroy(ui);
    return 0;
}
