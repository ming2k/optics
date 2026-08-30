/*
 * a11y_tree_demo.c — Demonstrates how to walk the lens accessibility tree.
 *
 * lens retains semantic information for every widget (roles, accessible names,
 * values, and states like focused/disabled). This demo builds a typical
 * settings panel and then walks the tree to print the semantic hierarchy.
 *
 * Build: meson setup build -Dexamples=true && ./build/examples/lens/a11y_tree_demo
 */

#include <lens/lens.h>
#include <stdio.h>

static void a11y_visitor(const lens_semantics *s, flux_rect bounds, lens_id id, lens_id parent,
                         void *user) {
    (void)bounds;
    (void)id;
    (void)parent;
    int *depth = (int *)user;

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
    case LENS_ROLE_DISCLOSURE:
        role_name = "DISCLOSURE";
        break;
    case LENS_ROLE_SCROLLAREA:
        role_name = "SCROLLAREA";
        break;
    case LENS_ROLE_TEXTFIELD:
        role_name = "TEXTFIELD";
        break;
    case LENS_ROLE_TEXTAREA:
        role_name = "TEXTAREA";
        break;
    case LENS_ROLE_MENU:
        role_name = "MENU";
        break;
    case LENS_ROLE_RADIO:
        role_name = "RADIO";
        break;
    case LENS_ROLE_DIALOG:
        role_name = "DIALOG";
        break;
    case LENS_ROLE_PROGRESS:
        role_name = "PROGRESS";
        break;
    case LENS_ROLE_TABLE:
        role_name = "TABLE";
        break;
    case LENS_ROLE_ROW:
        role_name = "ROW";
        break;
    case LENS_ROLE_MENUITEM:
        role_name = "MENUITEM";
        break;
    case LENS_ROLE_LINK:
        role_name = "LINK";
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
    if (s->flags & LENS_A11Y_EXPANDED)
        printf(" (expanded)");
    if (s->flags & LENS_A11Y_READONLY)
        printf(" (readonly)");
    if (s->flags & LENS_A11Y_SELECTED)
        printf(" (selected)");

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
    lens_column_begin(ui, &(lens_layout_opts){.pad = 16.0f, .gap = 8.0f});
    lens_label(ui, &(lens_label_opts){.text = "Preferences", .size = 20.0f});

    bool notifications = true;
    lens_checkbox(ui,
                  &(lens_checkbox_opts){.label = "Enable Notifications", .value = &notifications});

    float volume = 0.75f;
    lens_slider(ui,
                &(lens_slider_opts){.label = "Volume", .value = &volume, .min = 0.0f, .max = 1.0f});

    bool light_theme = false;
    bool dark_theme = true;
    lens_label(ui, &(lens_label_opts){.text = "Theme Selection:"});
    lens_row_begin(ui, NULL);
    lens_checkbox(ui, &(lens_checkbox_opts){.label = "Light",
                                            .value = &light_theme,
                                            .appearance = LENS_CHECKBOX_RADIO});
    lens_checkbox(ui, &(lens_checkbox_opts){.label = "Dark",
                                            .value = &dark_theme,
                                            .appearance = LENS_CHECKBOX_RADIO});
    lens_close(ui);

    lens_spacer(ui, 20.0f);
    lens_button(ui, &(lens_button_opts){.label = "Save Changes", .box = {.disabled = true}});
    lens_close(ui);

    lens_end(ui);

    printf("--- Accessibility Tree Dump ---\n");
    int depth = 0;
    lens_accessibility_walk(ui, a11y_visitor, &depth);
    printf("-------------------------------\n");

    lens_destroy(ui);
    return 0;
}
