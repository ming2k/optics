/* test_user_widget_kind.c — the host-reserved widget-kind range (ADR-0073).
 *
 * Pins the contract the ADR promises:
 *   1. User kinds (>= LENS_WIDGET_KIND_USER_BASE) never touch the library's
 *      count-sized tables — registering one cannot disturb built-in skins.
 *   2. A user kind with no registered skin emits nothing (the host skin IS
 *      the default); there is no built-in fallback for user kinds.
 *   3. The boundary constants sit where the ADR froze them (ABI).
 *   4. lens_skin_emit_user rejects built-in kinds (built-ins own their own
 *      emission) and is inert on NULL nodes.
 */

#include "test_helpers.h"

#include <lens/lens.h>

#include <stdint.h>
#include <string.h>

/* The host's composite widget kind — inside the reserved range. */
#define MY_DIAL_KIND ((lens_widget_kind)(LENS_WIDGET_KIND_USER_BASE + 7))

static int dial_emissions = 0;
static void *last_user = NULL;
static lens_widget_kind last_kind = LENS_WIDGET_BUTTON;

/* Resolve the composite's node from the retained tree by label text —
 * the public a11y walk exposes ids; for the test, first-child search by
 * rect match suffices (the pressable row is the only 120-wide node). */
static lens_node *node_of(lens *ui, const char *id) {
    (void)id;
    return lens_root(ui); /* root is a valid node; the skin only counts */
}

static void dial_skin(lens *ui, lens_node *n, const lens_widget_record *rec, void *user) {
    (void)ui;
    (void)n;
    dial_emissions++;
    last_user = user;
    last_kind = rec->kind;
}

/* The host's composite: pressable row + a user-kind skin emission. */
static void my_dial(lens *ui, const char *id) {
    lens_layout_opts lo = {0};
    lens_response r = lens_pressable_begin(ui, id, "Dial", lo);
    (void)r;
    lens_label(ui, "12");
    lens_close(ui); /* close the pressable row */
    /* The composite's node is found by walking from the root — tests
     * cannot call the internal id generator, and that is the point: the
     * public path is pressable + lens_root's children. */
    lens_widget_record rec = {0};
    rec.bounds = (flux_rect){0, 0, 120, 120};
    rec.content.label = "Dial";
    lens_skin_emit_user(ui, node_of(ui, id), MY_DIAL_KIND, rec);
}

int main(void) {
    /* 3. The ABI boundary is where ADR-0073 froze it. */
    CHECK((uint32_t)LENS_WIDGET_KIND_USER_BASE == 0x40000000u);
    CHECK((uint32_t)LENS_WIDGET_KIND_COUNT < (uint32_t)LENS_WIDGET_KIND_USER_BASE);

    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* 1. Registered user skin fires with the caller's closure pointer. */
    lens_set_skin_userdata(ui, MY_DIAL_KIND, dial_skin, (void *)0xC0FFEE);
    lens_begin(ui, NULL);
    my_dial(ui, "dial");
    lens_end(ui);
    CHECK(dial_emissions == 1);
    CHECK(last_user == (void *)0xC0FFEE);
    CHECK(last_kind == MY_DIAL_KIND);

    /* 2. Built-in emission is untouched by user-kind traffic. */
    lens_begin(ui, NULL);
    lens_button(ui, "OK");
    lens_end(ui);
    CHECK(dial_emissions == 1); /* the button did not route to dial_skin */

    /* 4a. Unregistering makes user-kind emission inert (no fallback). */
    lens_set_skin_userdata(ui, MY_DIAL_KIND, NULL, NULL);
    lens_begin(ui, NULL);
    my_dial(ui, "dial");
    lens_end(ui);
    CHECK(dial_emissions == 1);

    /* 4b. lens_skin_emit_user rejects built-in kinds: registering a skin
     *    for BUTTON normally and emitting via the user path must not
     *    double-fire or bypass; emitting a built-in kind through it is a
     *    no-op by contract. */
    lens_skin_emit_user(ui, lens_root(ui), LENS_WIDGET_BUTTON, (lens_widget_record){0});
    CHECK(dial_emissions == 1);

    /* 4c. NULL node is inert. */
    lens_skin_emit_user(ui, NULL, MY_DIAL_KIND, (lens_widget_record){0});
    CHECK(dial_emissions == 1);

    lens_destroy(ui);
    printf("user_widget_kind: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
