/* test_skin_userdata.c — the closure-slot skin registration. */

#include "lens_test.h"
#include "optics_test.h"

#include <lens/lens.h>

typedef struct {
    int emissions;
    float indicator;
} spring_state;

static void spring_skin(lens *ui, lens_node *n, const lens_widget_record *rec, void *user) {
    (void)ui;
    (void)n;
    (void)rec;
    spring_state *s = user;
    s->emissions++;
    s->indicator += 1.0f;
}

static int plain_emissions = 0;
static void plain_skin(lens *ui, lens_node *n, const lens_widget_record *rec) {
    (void)ui;
    (void)n;
    (void)rec;
    plain_emissions++;
}

int main(void) {
    lens *ui = NULL;
    OT_EXPECT(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    plain_emissions = 0;

    /* Frame 1: userdata skin receives its pointer per emission. */
    spring_state spring = {0, 0.0f};
    lens_set_skin_userdata(ui, LENS_WIDGET_BUTTON, spring_skin, &spring);
    lens_begin(ui, NULL);
    lens_button(ui, &(lens_button_opts){.label = "Click"});
    lens_test_end(ui);
    OT_EXPECT(spring.emissions >= 1);
    OT_EXPECT(spring.indicator >= 1.0f);

    /* The userdata pointer is stable across frames (stored verbatim). */
    int before = spring.emissions;
    lens_begin(ui, NULL);
    lens_button(ui, &(lens_button_opts){.label = "Click"});
    lens_test_end(ui);
    OT_EXPECT(spring.emissions > before);

    /* Plain registration supersedes the userdata form for that kind. */
    lens_set_skin(ui, LENS_WIDGET_BUTTON, plain_skin);
    lens_begin(ui, NULL);
    lens_button(ui, &(lens_button_opts){.label = "Click"});
    lens_test_end(ui);
    OT_EXPECT(plain_emissions >= 1);
    int frozen = spring.emissions;
    OT_EXPECT(frozen >= 1);

    /* NULL restores the built-in default for both forms. */
    lens_set_skin(ui, LENS_WIDGET_BUTTON, NULL);
    lens_set_skin_userdata(ui, LENS_WIDGET_BUTTON, NULL, NULL);
    lens_begin(ui, NULL);
    lens_button(ui, &(lens_button_opts){.label = "Click"});
    lens_test_end(ui);
    OT_EXPECT(plain_emissions == 1); /* no more plain-skin emissions */
    OT_EXPECT(spring.emissions == frozen);

    lens_release(ui);
    printf("skin_userdata: %d checks, %d failures\n", OT_CHECKS(), OT_FAILURES());
    return OT_FAILURES() ? 1 : 0;
}
