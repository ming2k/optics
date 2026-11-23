/* test_user_widget_kind.c — the host-reserved widget-kind range (ADR-0073). */

#include "optics_test.h"
#include "ui_test.h"

#include <optics/ui.h>

#include <stdint.h>
#include <string.h>

/* The host's composite widget kind — inside the reserved range. */
#define MY_DIAL_KIND ((optics_ui_widget_kind)(OPTICS_UI_WIDGET_KIND_USER_BASE + 7))

static int dial_emissions = 0;
static void *last_user = nullptr;
static optics_ui_widget_kind last_kind = OPTICS_UI_WIDGET_BUTTON;

static optics_ui_node *node_of(optics_ui *ui, const char *id) {
    (void)id;
    return optics_ui_root(ui);
}

static void dial_skin(optics_ui *ui, optics_ui_node *n, const optics_ui_widget_record *rec,
                      void *user) {
    (void)ui;
    (void)n;
    dial_emissions++;
    last_user = user;
    last_kind = rec->kind;
}

static void my_dial(optics_ui *ui, const char *id) {
    optics_ui_response r = optics_ui_pressable_begin(
        ui, &(optics_ui_pressable_opts){.box = {.id = id}, .label = "Dial"});
    (void)r;
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "12"});
    optics_ui_close(ui);
    optics_ui_widget_record rec = {};
    rec.bounds = (optics_gfx_rect){0, 0, 120, 120};
    rec.content.label = "Dial";
    optics_ui_skin_emit_user(ui, &(optics_ui_skin_emit_user_opts){
                                     .node = node_of(ui, id), .kind = MY_DIAL_KIND, .rec = rec});
}

int main(void) {
    OT_EXPECT((uint32_t)OPTICS_UI_WIDGET_KIND_USER_BASE == 0x40000000u);
    OT_EXPECT((uint32_t)OPTICS_UI_WIDGET_KIND_COUNT < (uint32_t)OPTICS_UI_WIDGET_KIND_USER_BASE);

    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    optics_ui_set_skin_userdata(ui, MY_DIAL_KIND, dial_skin, (void *)0xC0FFEE);
    optics_ui_begin(ui, nullptr);
    my_dial(ui, "dial");
    optics_ui_test_end(ui);
    OT_EXPECT(dial_emissions == 1);
    OT_EXPECT(last_user == (void *)0xC0FFEE);
    OT_EXPECT(last_kind == MY_DIAL_KIND);

    optics_ui_begin(ui, nullptr);
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "OK"});
    optics_ui_test_end(ui);
    OT_EXPECT(dial_emissions == 1);

    optics_ui_set_skin_userdata(ui, MY_DIAL_KIND, nullptr, nullptr);
    optics_ui_begin(ui, nullptr);
    my_dial(ui, "dial");
    optics_ui_test_end(ui);
    OT_EXPECT(dial_emissions == 1);

    optics_ui_skin_emit_user(ui, &(optics_ui_skin_emit_user_opts){
                                     .node = optics_ui_root(ui),
                                     .kind = OPTICS_UI_WIDGET_BUTTON,
                                     .rec = (optics_ui_widget_record){},
                                 });
    OT_EXPECT(dial_emissions == 1);

    optics_ui_skin_emit_user(ui, &(optics_ui_skin_emit_user_opts){
                                     .node = nullptr,
                                     .kind = MY_DIAL_KIND,
                                     .rec = (optics_ui_widget_record){},
                                 });
    OT_EXPECT(dial_emissions == 1);

    optics_ui_release(ui);
    printf("user_widget_kind: %d checks, %d failures\n", OT_CHECKS(), OT_FAILURES());
    return OT_FAILURES() ? 1 : 0;
}
