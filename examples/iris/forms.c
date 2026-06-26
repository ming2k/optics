/* forms.c — form validation and input widgets demo.
 *
 * A mock user-registration form showing text fields, dropdowns,
 * validation errors, tooltips, and a submit button.
 */

#include "app_shell.h"
#include <iris/app.h>
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

    lens_column_ex(ui,
                   (lens_layout_opts){.pad = 24, .gap = 8, .cross = LENS_STRETCH, .bg = tn.card});

    lens_size(ui, 0, 40);
    lens_label(ui, "Registration Form");
    lens_separator(ui);

    /* Each field carries its own error + tooltip in its descriptor, so
     * the validation state can only apply to the widget it belongs to. */

    /* Name */
    lens_label(ui, "Full name");
    bool name_err = a->submitted && strlen(a->name) == 0;
    if (lens_textfield_ex(
            ui, (lens_textfield_opts){.box = {.id = "name",
                                              .error = name_err,
                                              .tooltip = name_err ? "Name is required." : NULL},
                                      .buf = a->name,
                                      .buf_cap = sizeof a->name})
            .changed)
        a->submitted = false;

    /* Email */
    lens_label(ui, "Email address");
    bool email_err = a->submitted && !valid_email(a->email);
    if (lens_textfield_ex(
            ui,
            (lens_textfield_opts){
                .box = {.id = "email",
                        .error = email_err,
                        .tooltip = email_err ? "Please enter a valid email address." : NULL},
                .buf = a->email,
                .buf_cap = sizeof a->email})
            .changed)
        a->submitted = false;

    /* Country dropdown */
    lens_label(ui, "Country");
    bool country_err = a->submitted && a->country == 0;
    if (lens_dropdown_ex(ui,
                         (lens_dropdown_opts){
                             .box = {.id = "country",
                                     .error = country_err,
                                     .tooltip = country_err ? "Please select a country." : NULL},
                             .selected = &a->country,
                             .items = countries,
                             .count = (int)ARRAY_COUNT(countries)})
            .changed)
        a->submitted = false;

    /* Age slider */
    lens_label(ui, "Age");
    if (lens_slider_ex(
            ui,
            (lens_slider_opts){.box = {.id = "age"}, .value = &a->age, .min = 0.0f, .max = 120.0f})
            .changed)
        a->submitted = false;

    /* Bio textarea */
    lens_label(ui, "Bio (optional)");
    if (lens_textarea_ex(ui, (lens_textarea_opts){.box = {.id = "bio", .height = 100},
                                                  .buf = a->bio,
                                                  .buf_cap = sizeof a->bio,
                                                  .min_height = 60.0f})
            .changed)
        a->submitted = false;

    /* Terms checkbox */
    lens_size(ui, 0, 8);
    bool agree_err = a->submitted && !a->agree;
    if (lens_checkbox_ex(ui,
                         (lens_checkbox_opts){
                             .box = {.tooltip = agree_err ? "You must agree to continue." : NULL},
                             .label = "I agree to the terms and conditions",
                             .value = &a->agree})
            .changed)
        a->submitted = false;

    lens_size(ui, 0, 16);

    /* Submit button */
    if (lens_button(ui, "Submit")) {
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
        lens_label(ui, "Thank you! Your registration has been received.");
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
