/* forms.c — form validation and input widgets demo.
 *
 * A mock user-registration form showing text fields, selectables,
 * validation errors, tooltips, and a submit button.
 */

#include "app_shell.h"
#include <iris/app.h>
#include <iris/window.h>
#include <lens/lens.h>

#include <stdio.h>
#include <string.h>

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

typedef struct app {
    char name[64];
    char email[64];
    char bio[256];
    int country;
    bool agree;
    bool submitted;
    float age;
} app;

static const char *countries[] = {
    "Select a country...",
    "United States",
    "United Kingdom",
    "Germany",
    "France",
    "Japan",
    "China",
    "Other",
};

static bool valid_email(const char *s) {
    return strchr(s, '@') != NULL && strchr(s, '.') != NULL;
}

static void build(lens *ui, const lens_input *in, void *user) {
    app *a = user;
    lens_theme th = lens_get_theme(ui);
    shell_tones tn = shell_tones_from(&th);

    /* Esc quits (the startup line says so). */
    for (uint32_t k = 0; k < in->key_count; k++)
        if (in->keys[k].pressed && in->keys[k].key == LENS_KEY_ESCAPE)
            iris_window_close();

    lens_column_begin(
        ui, &(lens_layout_opts){.pad = 24, .gap = 8, .cross = LENS_STRETCH, .bg = tn.card});

    lens_size(ui, 0, 40);
    lens_label(ui, &(lens_label_opts){.text = "Registration Form", .size = 20.0f});
    lens_separator(ui, NULL);

    /* Name */
    lens_label(ui, &(lens_label_opts){.text = "Full name"});
    bool name_err = a->submitted && strlen(a->name) == 0;
    if (lens_textedit(ui,
                      &(lens_textedit_opts){
                          .box = {.id = "name",
                                  .error = name_err,
                                  .tooltip = name_err ? "Name is required." : NULL},
                          .buf = a->name,
                          .cap = sizeof a->name,
                      })
            .changed)
        a->submitted = false;

    /* Email */
    lens_label(ui, &(lens_label_opts){.text = "Email address"});
    bool email_err = a->submitted && !valid_email(a->email);
    if (lens_textedit(
            ui,
            &(lens_textedit_opts){
                .box = {.id = "email",
                        .error = email_err,
                        .tooltip = email_err ? "Please enter a valid email address." : NULL},
                .buf = a->email,
                .cap = sizeof a->email,
            })
            .changed)
        a->submitted = false;

    /* Country dropdown via button + place popup */
    lens_label(ui, &(lens_label_opts){.text = "Country"});
    bool country_err = a->submitted && a->country == 0;
    if (lens_button(ui,
                    &(lens_button_opts){
                        .box = {.id = "country_btn",
                                .error = country_err,
                                .tooltip = country_err ? "Please select a country." : NULL},
                        .label = countries[a->country],
                    })
            .clicked) {
        lens_place_toggle(ui, "country_menu");
    }

    if (lens_place_begin(ui, &(lens_place_opts){
                                 .box = {.id = "country_menu"},
                                 .band = LENS_BAND_POPUP,
                                 .mode = LENS_PLACE_ANCHORED,
                                 .transient = true,
                                 .layout = {.bg = tn.card,
                                            .border = th.color_border,
                                            .border_width = 1.0f,
                                            .pad = 4.0f,
                                            .gap = 2.0f},
                             })) {
        for (int i = 0; i < (int)ARRAY_COUNT(countries); i++) {
            if (lens_selectable(ui, &(lens_selectable_opts){.label = countries[i],
                                                            .selected = (a->country == i)})
                    .clicked) {
                a->country = i;
                a->submitted = false;
                lens_place_close(ui, "country_menu");
            }
        }
        lens_place_end(ui);
    }

    /* Age slider */
    lens_label(ui, &(lens_label_opts){.text = "Age"});
    if (lens_slider(
            ui, &(lens_slider_opts){.label = "age", .value = &a->age, .min = 0.0f, .max = 120.0f})
            .changed)
        a->submitted = false;

    /* Bio textarea */
    lens_label(ui, &(lens_label_opts){.text = "Bio (optional)"});
    if (lens_textedit(ui, &(lens_textedit_opts){.box = {.id = "bio", .height = 100},
                                                .buf = a->bio,
                                                .cap = sizeof a->bio,
                                                .rows = 4})
            .changed)
        a->submitted = false;

    /* Terms checkbox */
    lens_size(ui, 0, 8);
    bool agree_err = a->submitted && !a->agree;
    if (lens_checkbox(ui,
                      &(lens_checkbox_opts){
                          .box = {.tooltip = agree_err ? "You must agree to continue." : NULL},
                          .label = "I agree to the terms and conditions",
                          .value = &a->agree,
                      })
            .changed)
        a->submitted = false;

    lens_size(ui, 0, 16);

    /* Submit button */
    if (lens_button(ui, &(lens_button_opts){.label = "Submit", .variant = LENS_BUTTON_PRIMARY})
            .clicked) {
        a->submitted = true;
        bool ok = strlen(a->name) > 0 && valid_email(a->email) && a->country > 0 && a->agree;
        if (ok)
            printf("Form submitted: %s <%s>\n", a->name, a->email);
        else
            printf("Form has errors. Please fix them.\n");
    }

    /* Success message */
    if (a->submitted && strlen(a->name) > 0 && valid_email(a->email) && a->country > 0 &&
        a->agree) {
        lens_size(ui, 0, 8);
        lens_label(ui,
                   &(lens_label_opts){.text = "Thank you! Your registration has been received."});
    }

    lens_close(ui);
}

int main(void) {
    app a = {.age = 25.0f};
    printf("iris forms demo. Esc quits.\n\n");
    return iris_app_run(&(iris_app_config){
        .title = "iris — forms",
        .width = 480,
        .height = 720,
        .dark = true,
        .build = build,
        .user = &a,
    });
}
