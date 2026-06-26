/*
 * a11y_tree_demo.c — Demonstrates how to walk the lens accessibility tree.
 *
 * lens retains semantic information for every widget (roles, accessible names,
 * values, and states like focused/disabled). This demo builds a typical
 * settings panel and then walks the tree to print the semantic hierarchy,
 * which is exactly what a platform AT-SPI or UIA bridge would do.
 *
 * Build: meson setup build -Dexamples=true && ./build/examples/lens/a11y_tree_demo
 */

#include <lens/lens.h>
#include <stdio.h>

static void a11y_visitor(const lens_semantics *s, flux_rect bounds, lens_id id, lens_id parent,
                         void *user) {
    int *depth = (int *)user;

    /* Indent based on depth (this is a simplified linear walk,
     * but the parent ID allows building a true tree). */
    for (int i = 0; i < *depth; i++)
        printf("  ");

    const char *role_name = "UNKNOWN";
    switch (s->role) {
    case LENS_ROLE_NONE:
        role_name = "NONE";
        break;
    case LENS_ROLE_GROUP:
        role_name = "GROUP";
        break;
    case LENS_ROLE_LABEL:
        role_name = "LABEL";
        break;
    case LENS_ROLE_BUTTON:
        role_name = "BUTTON";
        break;
    case LENS_ROLE_CHECKBOX:
        role_name = "CHECKBOX";
        break;
    case LENS_ROLE_SLIDER:
        role_name = "SLIDER";
        break;
    case LENS_ROLE_TEXTFIELD:
        role_name = "TEXTFIELD";
        break;
    case LENS_ROLE_RADIO:
        role_name = "RADIO";
        break;
    case LENS_ROLE_DIALOG:
        role_name = "DIALOG";
        break;
    default:
        break;
    }

    printf("[%s] name=\"%s\"", role_name, s->name ? s->name : "");
    if (s->value) {
        printf(" value=\"%s\"", s->value);
    }

    if (s->flags & LENS_A11Y_DISABLED)
        printf(" (disabled)");
    if (s->flags & LENS_A11Y_FOCUSED)
        printf(" (focused)");
    if (s->flags & LENS_A11Y_CHECKED)
        printf(" (checked)");

    printf("\n");
}

int main(void) {
    lens *ui = NULL;
    if (lens_create(&(lens_desc){.theme = lens_theme_default()}, &ui) != FLUX_OK) {
        return 1;
    }

    /* Simulate a single frame */
    lens_input in = {.display_size = {800, 600}, .dt_seconds = 1.0f / 60.0f};
    lens_begin(ui, &in);

    /* Build a mock settings dialog */
    lens_column_ex(ui, (lens_layout_opts){.pad = 16.0f, .gap = 8.0f});
    lens_title(ui, "Preferences");

    bool notifications = true;
    lens_checkbox(ui, "Enable Notifications", &notifications);

    float volume = 0.75f;
    lens_slider(ui, "Volume", &volume, 0.0f, 1.0f);

    int theme = 1;
    lens_label(ui, "Theme Selection:");
    lens_row(ui);
    lens_radio(ui, "Light", &theme, 0);
    lens_radio(ui, "Dark", &theme, 1);
    lens_close(ui);

    lens_spacer(ui, 20.0f);
    lens_button_ex(ui, (lens_button_opts){.label = "Save Changes", .box = {.disabled = true}});
    lens_close(ui);

    lens_end(ui);

    printf("--- Accessibility Tree Dump ---\n");
    int depth = 0;
    lens_accessibility_walk(ui, a11y_visitor, &depth);
    printf("-------------------------------\n");

    lens_destroy(ui);
    return 0;
}
