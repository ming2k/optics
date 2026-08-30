/* test_user_widget_kind.c — the host-reserved widget-kind range (ADR-0073). */

#include "test_helpers.h"

#include <lens/lens.h>

#include <stdint.h>
#include <string.h>

/* The host's composite widget kind — inside the reserved range. */
#define MY_DIAL_KIND ((lens_widget_kind)(LENS_WIDGET_KIND_USER_BASE + 7))

static int dial_emissions = 0;
static void *last_user = NULL;
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
    lens_widget_record rec = {0};
    rec.bounds = (flux_rect){0, 0, 120, 120};
    rec.content.label = "Dial";
    lens_skin_emit_user(ui, node_of(ui, id), MY_DIAL_KIND, rec);
}

int main(void) {
    CHECK((uint32_t)LENS_WIDGET_KIND_USER_BASE == 0x40000000u);
    CHECK((uint32_t)LENS_WIDGET_KIND_COUNT < (uint32_t)LENS_WIDGET_KIND_USER_BASE);

    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_set_skin_userdata(ui, MY_DIAL_KIND, dial_skin, (void *)0xC0FFEE);
    lens_begin(ui, NULL);
    my_dial(ui, "dial");
    lens_end(ui);
    CHECK(dial_emissions == 1);
    CHECK(last_user == (void *)0xC0FFEE);
    CHECK(last_kind == MY_DIAL_KIND);

    lens_begin(ui, NULL);
    lens_button(ui, &(lens_button_opts){.label = "OK"});
    lens_end(ui);
    CHECK(dial_emissions == 1);

    lens_set_skin_userdata(ui, MY_DIAL_KIND, NULL, NULL);
    lens_begin(ui, NULL);
    my_dial(ui, "dial");
    lens_end(ui);
    CHECK(dial_emissions == 1);

    lens_skin_emit_user(ui, lens_root(ui), LENS_WIDGET_BUTTON, (lens_widget_record){0});
    CHECK(dial_emissions == 1);

    lens_skin_emit_user(ui, NULL, MY_DIAL_KIND, (lens_widget_record){0});
    CHECK(dial_emissions == 1);

    lens_destroy(ui);
    printf("user_widget_kind: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
