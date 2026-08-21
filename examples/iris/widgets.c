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
#include <iris/window.h>
#include <lens/lens.h>

#include <stdio.h>

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

typedef struct app {
    bool enable, wrap, dark;
    float volume, zoom, progress;
    int counter, theme_sel, tab_active, nav_sel;
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

    /* Esc quits (the startup line says so). */
    for (uint32_t k = 0; k < in->key_count; k++)
        if (in->keys[k].pressed && in->keys[k].key == LENS_KEY_ESCAPE)
            iris_window_close();

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
    /* The bar animates on its own: without a per-frame request the backend
     * drops to the idle cadence (or stops scheduling frames entirely) and
     * the bar only advances when the user wiggles something. */
    iris_request_animation_frame();
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

    /* ── Style cascade (ADR-0061) ────────────────────────── */
    section(ui, "Style cascade (scope + box.style)");
    lens_label(ui, "A lens_push_style scope restyles everything declared inside it:");
    row(ui, "Scoped row");
    lens_style danger = lens_style_init();
    danger.fields = LENS_STYLE_BG | LENS_STYLE_CORNER_RADIUS;
    danger.bg = flux_color_rgba(0xC0, 0x30, 0x28, 0xFF);
    danger.corner_radius = 2.0f;
    lens_push_style(ui, danger);
    if (lens_button(ui, "Delete"))
        printf("delete\n");
    if (lens_button(ui, "Also danger"))
        printf("also danger\n");
    lens_pop_style(ui);
    if (lens_button(ui, "Back to theme"))
        printf("themed\n");
    lens_close(ui);

    /* The icon-button active state is a neutral tint by default. The old
     * accent treatment is reachable as data — a scope supplies the atoms
     * (accent glyph + stronger active tile), the widget family stays free
     * of per-variant APIs (ADR-0061 item 7). */
    lens_label(ui, "Active icon button, accent treatment via scope atoms:");
    row(ui, "Nav strip");
    lens_style nav = lens_style_init();
    nav.fields = LENS_STYLE_FG | LENS_STYLE_BG_PRESSED;
    nav.fg = th.color_accent;                                 /* glyph colour at rest and active */
    nav.bg_pressed = flux_color_rgba(0x3A, 0x6A, 0xC0, 0x30); /* active tile tint */
    lens_push_style(ui, nav);
    if (lens_icon_button_active(ui, LENS_ICON_HOME, a->nav_sel == 0))
        a->nav_sel = 0;
    if (lens_icon_button_active(ui, LENS_ICON_GLOBE, a->nav_sel == 1))
        a->nav_sel = 1;
    if (lens_icon_button_active(ui, LENS_ICON_SETTINGS, a->nav_sel == 2))
        a->nav_sel = 2;
    lens_pop_style(ui);
    lens_close(ui);

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
