/* test_user_widget_kind.c — the host-reserved widget-kind range (ADR-0073). */

#include "lens_test.h"
#include "optics_test.h"

#include <lens/lens.h>

#include <stdint.h>
#include <string.h>

/* The host's composite widget kind — inside the reserved range. */
#define MY_DIAL_KIND ((lens_widget_kind)(LENS_WIDGET_KIND_USER_BASE + 7))

static int dial_emissions = 0;
static void *last_user = nullptr;
static lens_widget_kind last_kind = LENS_WIDGET_BUTTON;

static lens_node *node_of(lens *ui, const char *id) {
    (void)id;
    return lens_root(ui);
}

static void dial_skin(lens *ui, lens_node *n, const lens_widget_record *rec, void *user) {
    (void)ui;
    (void)n;
    dial_emissions++;
    last_user = user;
    last_kind = rec->kind;
}

static void my_dial(lens *ui, const char *id) {
    lens_response r =
        lens_pressable_begin(ui, &(lens_pressable_opts){.box = {.id = id}, .label = "Dial"});
    (void)r;
    lens_label(ui, &(lens_label_opts){.text = "12"});
    lens_close(ui);
    lens_widget_record rec = {};
    rec.bounds = (flux_rect){0, 0, 120, 120};
    rec.content.label = "Dial";
    lens_skin_emit_user(
        ui, &(lens_skin_emit_user_opts){.node = node_of(ui, id), .kind = MY_DIAL_KIND, .rec = rec});
}

int main(void) {
    OT_EXPECT((uint32_t)LENS_WIDGET_KIND_USER_BASE == 0x40000000u);
    OT_EXPECT((uint32_t)LENS_WIDGET_KIND_COUNT < (uint32_t)LENS_WIDGET_KIND_USER_BASE);

    lens *ui = nullptr;
    OT_EXPECT(lens_create(&(lens_desc){}, &ui) == FLUX_OK);

    lens_set_skin_userdata(ui, MY_DIAL_KIND, dial_skin, (void *)0xC0FFEE);
    lens_begin(ui, nullptr);
    my_dial(ui, "dial");
    lens_test_end(ui);
    OT_EXPECT(dial_emissions == 1);
    OT_EXPECT(last_user == (void *)0xC0FFEE);
    OT_EXPECT(last_kind == MY_DIAL_KIND);

    lens_begin(ui, nullptr);
    lens_button(ui, &(lens_button_opts){.label = "OK"});
    lens_test_end(ui);
    OT_EXPECT(dial_emissions == 1);

    lens_set_skin_userdata(ui, MY_DIAL_KIND, nullptr, nullptr);
    lens_begin(ui, nullptr);
    my_dial(ui, "dial");
    lens_test_end(ui);
    OT_EXPECT(dial_emissions == 1);

    lens_skin_emit_user(ui, &(lens_skin_emit_user_opts){
                                .node = lens_root(ui),
                                .kind = LENS_WIDGET_BUTTON,
                                .rec = (lens_widget_record){},
                            });
    OT_EXPECT(dial_emissions == 1);

    lens_skin_emit_user(ui, &(lens_skin_emit_user_opts){
                                .node = nullptr,
                                .kind = MY_DIAL_KIND,
                                .rec = (lens_widget_record){},
                            });
    OT_EXPECT(dial_emissions == 1);

    lens_release(ui);
    printf("user_widget_kind: %d checks, %d failures\n", OT_CHECKS(), OT_FAILURES());
    return OT_FAILURES() ? 1 : 0;
}
