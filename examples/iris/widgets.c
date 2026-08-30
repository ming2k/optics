/* widgets.c — every iris widget in one labeled reference.
 *
 * Interactions print to the console. Esc quits.
 */

#include "app_shell.h"
#include <iris/app.h>
#include <iris/window.h>
#include <lens/lens.h>

#include <stdio.h>

typedef struct app {
    bool enable, wrap, dark;
    float volume, zoom, progress;
    int counter, theme_sel, tab_active, nav_sel;
    char username[64];
    char bio[256];
    bool collapsing_open;
} app;

/* Section header: a label with a touch of breathing room above it. */
static void section(lens *ui, const char *title) {
    lens_size(ui, 0, 30);
    lens_label(ui, &(lens_label_opts){.text = title, .size = 18.0f});
}

/* Open a "label : control" row. Caller fills the control, then lens_close. */
static void row(lens *ui, const char *label) {
    lens_size(ui, 0, 30);
    lens_row_begin(ui, &(lens_layout_opts){.gap = 12, .cross = LENS_CENTER});
    lens_size(ui, 130, 24);
    lens_label(ui, &(lens_label_opts){.text = label});
}

static void build(lens *ui, const lens_input *in, void *user) {
    app *a = user;
    lens_theme th = lens_get_theme(ui);
    shell_tones tn = shell_tones_from(&th);

    /* Esc quits (the startup line says so). */
    for (uint32_t k = 0; k < in->key_count; k++)
        if (in->keys[k].pressed && in->keys[k].key == LENS_KEY_ESCAPE)
            iris_window_close();

    lens_flex(ui, 1.0f);
    lens_scroll_begin(ui, &(lens_scroll_opts){.box = {.id = "root_scroll"}});
    lens_column_begin(
        ui, &(lens_layout_opts){.pad = 22, .gap = 6, .cross = LENS_STRETCH, .bg = tn.card});

    lens_size(ui, 0, 34);
    lens_label(ui, &(lens_label_opts){.text = "iris widget reference", .size = 22.0f});
    lens_separator(ui, NULL);

    /* ── Buttons ─────────────────────────────────────────── */
    section(ui, "Buttons");
    row(ui, "Actions");
    if (lens_button(ui, &(lens_button_opts){.label = "New"}).clicked)
        printf("New\n");
    if (lens_button(ui, &(lens_button_opts){.label = "Open"}).clicked)
        printf("Open\n");
    if (lens_button(ui, &(lens_button_opts){.label = "Save", .variant = LENS_BUTTON_PRIMARY})
            .clicked)
        printf("Save\n");
    lens_close(ui);

    row(ui, "Counter");
    if (lens_button(ui, &(lens_button_opts){.label = "Increment"}).clicked)
        a->counter++;
    char n[32];
    snprintf(n, sizeof n, "count = %d", a->counter);
    lens_label(ui, &(lens_label_opts){.text = n});
    lens_close(ui);

    /* ── Checkboxes ──────────────────────────────────────── */
    section(ui, "Checkboxes");
    row(ui, "Enable feature");
    if (lens_checkbox(ui, &(lens_checkbox_opts){.label = "Enable", .value = &a->enable}).changed)
        printf("enable = %d\n", a->enable);
    lens_close(ui);

    row(ui, "Dark theme");
    if (lens_checkbox(ui, &(lens_checkbox_opts){.label = "Dark",
                                                .value = &a->dark,
                                                .appearance = LENS_CHECKBOX_SWITCH})
            .changed)
        lens_set_theme(ui, a->dark ? lens_theme_dark() : lens_theme_default());
    lens_close(ui);

    /* ── Radio buttons ───────────────────────────────────── */
    section(ui, "Radio buttons");
    row(ui, "Theme");
    bool opt_light = (a->theme_sel == 0);
    bool opt_dark = (a->theme_sel == 1);
    bool opt_auto = (a->theme_sel == 2);
    if (lens_checkbox(ui, &(lens_checkbox_opts){.label = "Light",
                                                .value = &opt_light,
                                                .appearance = LENS_CHECKBOX_RADIO})
            .changed)
        a->theme_sel = 0;
    if (lens_checkbox(ui, &(lens_checkbox_opts){.label = "Dark",
                                                .value = &opt_dark,
                                                .appearance = LENS_CHECKBOX_RADIO})
            .changed)
        a->theme_sel = 1;
    if (lens_checkbox(ui, &(lens_checkbox_opts){.label = "Auto",
                                                .value = &opt_auto,
                                                .appearance = LENS_CHECKBOX_RADIO})
            .changed)
        a->theme_sel = 2;
    lens_close(ui);

    /* ── Textfield ───────────────────────────────────────── */
    section(ui, "Textfield");
    row(ui, "Username");
    lens_flex(ui, 1.0f);
    if (lens_textedit(ui, &(lens_textedit_opts){.box = {.id = "user"},
                                                .buf = a->username,
                                                .cap = sizeof a->username})
            .changed)
        printf("username = %s\n", a->username);
    lens_close(ui);

    /* ── Sliders ─────────────────────────────────────────── */
    section(ui, "Sliders");
    row(ui, "Volume");
    lens_flex(ui, 1.0f);
    if (lens_slider(
            ui, &(lens_slider_opts){.label = "vol", .value = &a->volume, .min = 0.0f, .max = 1.0f})
            .changed)
        printf("volume = %.2f\n", a->volume);
    lens_close(ui);

    row(ui, "Zoom");
    lens_flex(ui, 1.0f);
    if (lens_slider(
            ui, &(lens_slider_opts){.label = "zoom", .value = &a->zoom, .min = 0.5f, .max = 4.0f})
            .changed)
        printf("zoom = %.2f\n", a->zoom);
    lens_close(ui);

    /* ── Progress indicator (custom box / bar composition) ── */
    section(ui, "Progress indicator (composed)");
    row(ui, "Loading");
    lens_flex(ui, 1.0f);
    a->progress += in->dt_seconds * 0.2f;
    if (a->progress > 1.0f)
        a->progress = 0.0f;
    lens_row_begin(
        ui, &(lens_layout_opts){.box = {.height = 10.0f}, .bg = 0xFF303038u, .radius = 5.0f});
    lens_size(ui, a->progress * 200.0f, 10.0f);
    lens_row_begin(ui, &(lens_layout_opts){.bg = th.color_accent, .radius = 5.0f});
    lens_close(ui);
    lens_close(ui);
    iris_request_animation_frame();
    lens_close(ui);

    /* ── Tabs (built with row + selectables) ─────────────── */
    section(ui, "Tabs");
    lens_row_begin(ui, &(lens_layout_opts){.gap = 8});
    if (lens_selectable(
            ui, &(lens_selectable_opts){.label = "General", .selected = (a->tab_active == 0)})
            .clicked)
        a->tab_active = 0;
    if (lens_selectable(
            ui, &(lens_selectable_opts){.label = "Advanced", .selected = (a->tab_active == 1)})
            .clicked)
        a->tab_active = 1;
    if (lens_selectable(ui,
                        &(lens_selectable_opts){.label = "About", .selected = (a->tab_active == 2)})
            .clicked)
        a->tab_active = 2;
    lens_close(ui);

    if (a->tab_active == 0)
        lens_label(ui, &(lens_label_opts){.text = "General settings panel."});
    else if (a->tab_active == 1)
        lens_label(ui, &(lens_label_opts){.text = "Advanced settings panel."});
    else
        lens_label(ui, &(lens_label_opts){.text = "About this application."});

    /* ── Textarea ────────────────────────────────────────── */
    section(ui, "Textarea");
    if (lens_textedit(ui, &(lens_textedit_opts){.box = {.id = "bio", .height = 120},
                                                .buf = a->bio,
                                                .cap = sizeof a->bio,
                                                .placeholder = "Tell us about yourself...",
                                                .rows = 4})
            .changed)
        printf("bio changed\n");

    /* ── Tooltip demo ────────────────────────────────────── */
    section(ui, "Tooltip");
    if (lens_button(ui,
                    &(lens_button_opts){.label = "Hover for tip",
                                        .box = {.tooltip = "This is a tooltip shown on hover."}})
            .clicked)
        printf("button clicked\n");

    /* ── Label ───────────────────────────────────────────── */
    section(ui, "Label");
    lens_label(ui, &(lens_label_opts){.text = "A static, non-interactive line of text."});

    /* ── Collapsing via button + state ───────────────────── */
    section(ui, "Collapsible Section");
    if (lens_button(
            ui, &(lens_button_opts){.label = a->collapsing_open ? "[-] Details" : "[+] Details"})
            .clicked)
        a->collapsing_open = !a->collapsing_open;
    if (a->collapsing_open) {
        lens_column_begin(ui, &(lens_layout_opts){.pad = 8.0f});
        lens_label(ui, &(lens_label_opts){.text = "Revealed when the header is expanded."});
        if (lens_button(ui, &(lens_button_opts){.label = "Nested button"}).clicked)
            printf("nested\n");
        lens_close(ui);
    }

    /* ── Scroll ──────────────────────────────────────────── */
    section(ui, "Scroll area (wheel over it)");
    lens_size(ui, 0, 150);
    lens_scroll_begin(ui, &(lens_scroll_opts){.box = {.id = "scroll"}});
    lens_column_begin(ui, &(lens_layout_opts){.pad = 12, .gap = 6, .cross = LENS_STRETCH});
    for (int i = 0; i < 14; i++) {
        char l[24];
        snprintf(l, sizeof l, "Row %d", i + 1);
        if (lens_button(ui, &(lens_button_opts){.label = l}).clicked)
            printf("%s\n", l);
    }
    lens_close(ui);
    lens_scroll_end(ui);

    lens_close(ui);
    lens_scroll_end(ui);
}

int main(void) {
    app a = {
        .dark = true,
        .volume = 0.6f,
        .zoom = 1.0f,
        .username = "",
        .bio = "",
    };
    printf("iris widgets reference. Interactions print here. Esc quits.\n\n");
    return iris_app_run(&(iris_app_config){
        .title = "iris — widgets",
        .width = 560,
        .height = 900,
        .dark = true,
        .build = build,
        .user = &a,
    });
}
