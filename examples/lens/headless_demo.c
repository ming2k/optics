/*
 * headless_demo.c — drive the lens frame loop with no window or GPU.
 *
 * Builds the same UI for a few frames against synthetic input and prints
 * the resolved layout and interaction. This exercises everything except
 * the canvas replay (which needs a flux_device + flux_canvas). For
 * windowed, interactive examples see the examples/iris/ directory.
 *
 * Build: meson setup build -Dexamples=true && ./build/examples/headless_demo
 */

#include <lens/lens.h>
#include <stdio.h>

static void build_ui(lens *ui, bool *wrap, float *zoom, int *clicks) {
    lens_label(ui, "lens headless demo");

    lens_row(ui);
    if (lens_button(ui, "New"))
        (*clicks)++;
    if (lens_button(ui, "Open"))
        (*clicks)++;
    lens_flex(ui, 1.0f);
    lens_spacer(ui, 0); /* push the rest right */
    lens_checkbox(ui, "Wrap", wrap);
    lens_close(ui);

    lens_slider(ui, "Zoom", zoom, 0.5f, 4.0f);
}

int main(void) {
    lens *ui = NULL;
    if (lens_create(&(lens_desc){.theme = lens_theme_dark()}, &ui) != FLUX_OK) {
        fprintf(stderr, "lens_create failed\n");
        return 1;
    }

    bool wrap = false;
    float zoom = 1.0f;
    int clicks = 0;

    /* A click is press then release across frames (ADR-0006). The "New"
     * button lands in the row at y≈42..78, x≈0..41 once laid out. */
    for (int frame = 0; frame < 4; frame++) {
        lens_input in = {.display_size = {480, 200}, .dt_seconds = 1.0f / 60.0f};
        if (frame == 2) { /* press over "New" */
            in.cursor = (flux_point){20, 55};
            in.mouse_down[LENS_MOUSE_LEFT] = true;
            in.mouse_pressed[LENS_MOUSE_LEFT] = true;
        } else if (frame == 3) { /* release over "New" */
            in.cursor = (flux_point){20, 55};
            in.mouse_released[LENS_MOUSE_LEFT] = true;
        }

        lens_begin(ui, &in);
        build_ui(ui, &wrap, &zoom, &clicks);
        lens_end(ui);

        printf("-- frame %d --  overflow=%d\n", frame, lens_overflowed(ui));
        lens_node *root = lens_root(ui);
        for (lens_node *c = lens_node_first_child(root); c; c = lens_node_next_sibling(c)) {
            flux_rect r = lens_node_bounds(c);
            printf("   node %016llx  rect={%.1f,%.1f,%.1f,%.1f}\n",
                   (unsigned long long)lens_node_id(c), r.x, r.y, r.w, r.h);
        }
    }

    printf("clicks=%d wrap=%d zoom=%.2f\n", clicks, wrap, zoom);
    lens_destroy(ui);
    return 0;
}
