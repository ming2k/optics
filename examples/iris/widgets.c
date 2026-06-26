/* widgets.c — every iris widget in one labeled reference.
 *
 * A tidy single-window form: each control sits in a "label : widget" row,
 * grouped under section headers, so the whole widget set is visible and
 * self-describing at a glance.
 *
 * Interactions print to the console. Esc quits.
 */

#include "app_shell.h"
#include <iris/app.h>
#include <lens/lens.h>

#include <stdio.h>

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

typedef struct app {
    bool enable, wrap, dark;
    float volume, zoom, progress;
    int counter, theme_sel, tab_active;
    char username[64];
    char bio[256];
    const char *theme_items[3];
} app;

/* Section header: a label with a touch of breathing room above it. */
static void section(lens *ui, const char *title) {
    lens_size(ui, 0, 30);
    lens_label(ui, title);
}

/* Open a "label : control" row. Caller fills the control, then lens_close. */
static void row(lens *ui, const char *label) {
    lens_size(ui, 0, 30);
    lens_row_ex(ui, (lens_layout_opts){.gap = 12, .cross = LENS_CENTER});
    lens_size(ui, 130, 24);
    lens_label(ui, label);
}

static void build(lens *ui, const lens_input *in, void *user) {
    app *a = user;
    lens_theme th = lens_get_theme(ui);
    shell_tones tn = shell_tones_from(&th);

    lens_flex(ui, 1.0f);
    lens_scroll_begin(ui, "root_scroll");
    lens_column_ex(ui,
                   (lens_layout_opts){.pad = 22, .gap = 6, .cross = LENS_STRETCH, .bg = tn.card});

    lens_size(ui, 0, 34);
    lens_label(ui, "iris widget reference");
    lens_separator(ui);

    /* ── Buttons ─────────────────────────────────────────── */
    section(ui, "Buttons");
    row(ui, "Actions");
    if (lens_button(ui, "New"))
        printf("New\n");
    if (lens_button(ui, "Open"))
        printf("Open\n");
    if (lens_button(ui, "Save"))
        printf("Save\n");
    lens_close(ui);
    row(ui, "Counter");
    if (lens_button(ui, "Increment"))
        a->counter++;
    char n[32];
    snprintf(n, sizeof n, "count = %d", a->counter);
    lens_label(ui, n);
    lens_close(ui);

    /* ── Checkboxes ──────────────────────────────────────── */
    section(ui, "Checkboxes");
    row(ui, "Enable feature");
    if (lens_checkbox(ui, "##enable", &a->enable))
        printf("enable = %d\n", a->enable);
    lens_close(ui);
    row(ui, "Dark theme");
    if (lens_checkbox(ui, "##dark", &a->dark))
        lens_set_theme(ui, a->dark ? lens_theme_dark() : lens_theme_default());
    lens_close(ui);

    /* ── Radio buttons ───────────────────────────────────── */
    section(ui, "Radio buttons");
    row(ui, "Theme");
    if (lens_radio(ui, "Light", &a->theme_sel, 0))
        printf("theme = light\n");
    if (lens_radio(ui, "Dark", &a->theme_sel, 1))
        printf("theme = dark\n");
    if (lens_radio(ui, "Auto", &a->theme_sel, 2))
        printf("theme = auto\n");
    lens_close(ui);

    /* ── Dropdown ────────────────────────────────────────── */
    section(ui, "Dropdown");
    row(ui, "Theme");
    lens_flex(ui, 1.0f);
    if (lens_dropdown(ui, "##theme", &a->theme_sel, a->theme_items,
                      (int)ARRAY_COUNT(a->theme_items)))
        printf("dropdown = %d\n", a->theme_sel);
    lens_close(ui);

    /* ── Textfield ───────────────────────────────────────── */
    section(ui, "Textfield");
    row(ui, "Username");
    lens_flex(ui, 1.0f);
    if (lens_textfield(ui, "##user", a->username, sizeof a->username))
        printf("username = %s\n", a->username);
    lens_close(ui);

    /* ── Sliders ─────────────────────────────────────────── */
    section(ui, "Sliders");
    row(ui, "Volume");
    lens_flex(ui, 1.0f);
    if (lens_slider(ui, "##vol", &a->volume, 0.0f, 1.0f))
        printf("volume = %.2f\n", a->volume);
    lens_close(ui);
    row(ui, "Zoom");
    lens_flex(ui, 1.0f);
    if (lens_slider(ui, "##zoom", &a->zoom, 0.5f, 4.0f))
        printf("zoom = %.2f\n", a->zoom);
    lens_close(ui);

    /* ── Progress bar ────────────────────────────────────── */
    section(ui, "Progress bar");
    row(ui, "Loading");
    lens_flex(ui, 1.0f);
    a->progress += lens_dt(ui) * 0.2f;
    if (a->progress > 1.0f)
        a->progress = 0.0f;
    lens_progress(ui, "##prog", a->progress);
    lens_close(ui);

    /* ── Tabs ────────────────────────────────────────────── */
    section(ui, "Tabs");
    if (lens_tabs_begin(ui, "tabs", &a->tab_active)) {
        lens_tab(ui, "General");
        lens_tab(ui, "Advanced");
        lens_tab(ui, "About");
    }
    lens_tabs_end(ui);
    if (a->tab_active == 0)
        lens_label(ui, "General settings panel.");
    else if (a->tab_active == 1)
        lens_label(ui, "Advanced settings panel.");
    else
        lens_label(ui, "About this application.");

    /* ── Textarea ────────────────────────────────────────── */
    section(ui, "Textarea");
    if (lens_textarea_ex(ui, (lens_textarea_opts){.box = {.id = "bio", .height = 120},
                                                  .buf = a->bio,
                                                  .buf_cap = sizeof a->bio,
                                                  .min_height = 60.0f,
                                                  .placeholder = "Tell us about yourself..."})
            .changed)
        printf("bio changed\n");

    /* ── Tooltip demo ────────────────────────────────────── */
    section(ui, "Tooltip");
    if (lens_button_ex(ui,
                       (lens_button_opts){.label = "Hover for tip",
                                          .box = {.tooltip = "This is a tooltip shown on hover."}})
            .clicked)
        printf("button clicked\n");

    /* ── Label ───────────────────────────────────────────── */
    section(ui, "Label");
    lens_label(ui, "A static, non-interactive line of text.");

    /* ── Collapsing ──────────────────────────────────────── */
    section(ui, "Collapsing header");
    if (lens_collapsing(ui, "Details")) {
        lens_label(ui, "Revealed when the header is expanded.");
        if (lens_button(ui, "Nested button"))
            printf("nested\n");
        lens_close(ui);
    }

    /* ── Scroll ──────────────────────────────────────────── */
    section(ui, "Scroll area (wheel over it)");
    lens_size(ui, 0, 150);
    lens_scroll_begin(ui, "scroll");
    lens_column_ex(ui, (lens_layout_opts){.pad = 12, .gap = 6, .cross = LENS_STRETCH});
    for (int i = 0; i < 14; i++) {
        char l[24];
        snprintf(l, sizeof l, "Row %d", i + 1);
        if (lens_button(ui, l))
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
        .theme_items = {"Light", "Dark", "Auto"},
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
