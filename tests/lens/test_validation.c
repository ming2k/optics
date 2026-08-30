/* test_validation.c — error state (lens_box.error) on textfield and slider.
 *
 * The error flag's entire contract is that it reaches the skin as
 * `content.error` (the default skin renders theme.color_error as the
 * border). The earlier version of this file only asserted "no crash /
 * no overflow", which would pass even if the flag were dropped on the
 * floor — so each case now captures the emitted widget record through
 * the public skin-override seam (lens_set_skin) and asserts the flag
 * arrived, and did NOT arrive on widgets that never asked for it. */

#include "test_helpers.h"
#include <lens/lens.h>
#include <string.h>

static const lens_input IN0 = {.display_size = {400, 200}, .dt_seconds = 0.016f};

/* Captured records (file scope; C has no closures). */
static bool g_saw_error, g_saw_clean, g_saw_any;
static void capture(lens *ui, lens_node *n, const lens_widget_record *rec) {
    (void)ui;
    (void)n;
    g_saw_any = true;
    if (rec->content.error)
        g_saw_error = true;
    else
        g_saw_clean = true;
}

static void build_textfield_pair(lens *ui, char *buf_a, char *buf_b) {
    lens_begin(ui, &IN0);
    lens_textedit(
        ui, &(lens_textedit_opts){.box = {.id = "tf1", .error = true}, .buf = buf_a, .cap = 64});
    lens_textedit(
        ui, &(lens_textedit_opts){.box = {.id = "tf2"}, .buf = buf_b, .cap = 64}); /* no error */
    lens_end(ui);
}

static void test_textfield_error_reaches_skin(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_set_skin(ui, LENS_WIDGET_TEXTEDIT, capture);

    char a[64] = "test", b[64] = "other";
    g_saw_error = g_saw_clean = g_saw_any = false;
    build_textfield_pair(ui, a, b);

    CHECK(g_saw_any);   /* the skin override fired */
    CHECK(g_saw_error); /* tf1's record carried error = true */
    CHECK(g_saw_clean); /* tf2's record carried error = false */
    CHECK(!lens_overflowed(ui));

    lens_set_skin(ui, LENS_WIDGET_TEXTEDIT, NULL); /* restore */
    lens_destroy(ui);
}

static void test_slider_error_reaches_skin(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_set_skin(ui, LENS_WIDGET_SLIDER, capture);

    float val = 0.5f;
    lens_begin(ui, &IN0);
    lens_slider(ui,
                &(lens_slider_opts){
                    .label = "s", .value = &val, .min = 0.0f, .max = 1.0f, .box = {.error = true}});
    lens_end(ui);

    CHECK(g_saw_any);
    CHECK(g_saw_error);
    CHECK(!lens_overflowed(ui));

    lens_set_skin(ui, LENS_WIDGET_SLIDER, NULL);
    lens_destroy(ui);
}

static void test_error_scoped_to_its_own_widget(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_set_skin(ui, LENS_WIDGET_TEXTEDIT, capture);

    /* Error is carried by the descriptor, so it can only apply to the one
     * widget it is set on — the capture above asserts exactly one record
     * arrived with error set and one without ("leaks to the next call"
     * would flip tf2's record to error too and fail g_saw_clean). */
    char a[64] = "hello", b[64] = "world";
    g_saw_error = g_saw_clean = g_saw_any = false;
    build_textfield_pair(ui, a, b);
    CHECK(g_saw_any && g_saw_error && g_saw_clean);

    lens_set_skin(ui, LENS_WIDGET_TEXTEDIT, NULL);
    lens_destroy(ui);
}

int main(void) {
    test_textfield_error_reaches_skin();
    test_slider_error_reaches_skin();
    test_error_scoped_to_its_own_widget();
    return TEST_REPORT();
}
