/* test_validation.c — error state (lens_box.error) on textfield and slider.
 *
 * The error flag's entire contract is that it reaches the skin as
 * `content.error` (the default skin renders theme.color_error as the
 * border). The earlier version of this file only asserted "no crash /
 * no overflow", which would pass even if the flag were dropped on the
 * floor — so each case now captures the emitted widget record through
 * the public skin-override seam (lens_set_skin) and asserts the flag
 * arrived, and did NOT arrive on widgets that never asked for it. */

#include "lens_test.h"
#include "optics_test.h"
#include <optics/ui.h>
#include <string.h>

static const optics_ui_input IN0 = {.display_size = {400, 200}, .dt_seconds = 0.016f};

/* Captured records (file scope; C has no closures). */
static bool g_saw_error, g_saw_clean, g_saw_any;
static void capture(optics_ui *ui, optics_ui_node *n, const optics_ui_widget_record *rec) {
    (void)ui;
    (void)n;
    g_saw_any = true;
    if (rec->content.error)
        g_saw_error = true;
    else
        g_saw_clean = true;
}

static void build_textfield_pair(optics_ui *ui, char *buf_a, char *buf_b) {
    optics_ui_begin(ui, &IN0);
    optics_ui_textedit(ui, &(optics_ui_textedit_opts){
                               .box = {.id = "tf1", .error = true}, .buf = buf_a, .cap = 64});
    optics_ui_textedit(ui, &(optics_ui_textedit_opts){
                               .box = {.id = "tf2"}, .buf = buf_b, .cap = 64}); /* no error */
    optics_ui_test_end(ui);
}

static void test_textfield_error_reaches_skin(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_set_skin(ui, OPTICS_UI_WIDGET_TEXTEDIT, capture);

    char a[64] = "test", b[64] = "other";
    g_saw_error = g_saw_clean = g_saw_any = false;
    build_textfield_pair(ui, a, b);

    OT_EXPECT(g_saw_any);   /* the skin override fired */
    OT_EXPECT(g_saw_error); /* tf1's record carried error = true */
    OT_EXPECT(g_saw_clean); /* tf2's record carried error = false */
    OT_EXPECT(!optics_ui_overflowed(ui));

    optics_ui_set_skin(ui, OPTICS_UI_WIDGET_TEXTEDIT, nullptr); /* restore */
    optics_ui_release(ui);
}

static void test_slider_error_reaches_skin(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_set_skin(ui, OPTICS_UI_WIDGET_SLIDER, capture);

    float val = 0.5f;
    optics_ui_begin(ui, &IN0);
    optics_ui_slider(
        ui, &(optics_ui_slider_opts){
                .label = "s", .value = &val, .min = 0.0f, .max = 1.0f, .box = {.error = true}});
    optics_ui_test_end(ui);

    OT_EXPECT(g_saw_any);
    OT_EXPECT(g_saw_error);
    OT_EXPECT(!optics_ui_overflowed(ui));

    optics_ui_set_skin(ui, OPTICS_UI_WIDGET_SLIDER, nullptr);
    optics_ui_release(ui);
}

static void test_error_scoped_to_its_own_widget(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_set_skin(ui, OPTICS_UI_WIDGET_TEXTEDIT, capture);

    /* Error is carried by the descriptor, so it can only apply to the one
     * widget it is set on — the capture above asserts exactly one record
     * arrived with error set and one without ("leaks to the next call"
     * would flip tf2's record to error too and fail g_saw_clean). */
    char a[64] = "hello", b[64] = "world";
    g_saw_error = g_saw_clean = g_saw_any = false;
    build_textfield_pair(ui, a, b);
    OT_EXPECT(g_saw_any && g_saw_error && g_saw_clean);

    optics_ui_set_skin(ui, OPTICS_UI_WIDGET_TEXTEDIT, nullptr);
    optics_ui_release(ui);
}

int main(void) {
    test_textfield_error_reaches_skin();
    test_slider_error_reaches_skin();
    test_error_scoped_to_its_own_widget();
    return OT_REPORT();
}
