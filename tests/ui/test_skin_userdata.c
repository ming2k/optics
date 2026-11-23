/* test_skin_userdata.c — the closure-slot skin registration. */

#include "optics_test.h"
#include "ui_test.h"

#include <optics/ui.h>

typedef struct {
    int emissions;
    float indicator;
} spring_state;

static void spring_skin(optics_ui *ui, optics_ui_node *n, const optics_ui_widget_record *rec,
                        void *user) {
    (void)ui;
    (void)n;
    (void)rec;
    spring_state *s = user;
    s->emissions++;
    s->indicator += 1.0f;
}

static int plain_emissions = 0;
static void plain_skin(optics_ui *ui, optics_ui_node *n, const optics_ui_widget_record *rec) {
    (void)ui;
    (void)n;
    (void)rec;
    plain_emissions++;
}

int main(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    plain_emissions = 0;

    /* Frame 1: userdata skin receives its pointer per emission. */
    spring_state spring = {0, 0.0f};
    optics_ui_set_skin_userdata(ui, OPTICS_UI_WIDGET_BUTTON, spring_skin, &spring);
    optics_ui_begin(ui, nullptr);
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "Click"});
    optics_ui_test_end(ui);
    OT_EXPECT(spring.emissions >= 1);
    OT_EXPECT(spring.indicator >= 1.0f);

    /* The userdata pointer is stable across frames (stored verbatim). */
    int before = spring.emissions;
    optics_ui_begin(ui, nullptr);
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "Click"});
    optics_ui_test_end(ui);
    OT_EXPECT(spring.emissions > before);

    /* Plain registration supersedes the userdata form for that kind. */
    optics_ui_set_skin(ui, OPTICS_UI_WIDGET_BUTTON, plain_skin);
    optics_ui_begin(ui, nullptr);
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "Click"});
    optics_ui_test_end(ui);
    OT_EXPECT(plain_emissions >= 1);
    int frozen = spring.emissions;
    OT_EXPECT(frozen >= 1);

    /* NULL restores the built-in default for both forms. */
    optics_ui_set_skin(ui, OPTICS_UI_WIDGET_BUTTON, nullptr);
    optics_ui_set_skin_userdata(ui, OPTICS_UI_WIDGET_BUTTON, nullptr, nullptr);
    optics_ui_begin(ui, nullptr);
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "Click"});
    optics_ui_test_end(ui);
    OT_EXPECT(plain_emissions == 1); /* no more plain-skin emissions */
    OT_EXPECT(spring.emissions == frozen);

    optics_ui_release(ui);
    printf("skin_userdata: %d checks, %d failures\n", OT_CHECKS(), OT_FAILURES());
    return OT_FAILURES() ? 1 : 0;
}
