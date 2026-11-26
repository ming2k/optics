/*
 * test_icon_info.c — optics_ui_icon_info() public read access to built-in and
 * per-context registered vector icons.
 */

#include "optics_test.h"
#include "ui_test.h"
#include <optics/ui.h>
#include <optics/ui/icon.h>

static const optics_ui_icon_cmd k_box_cmds[] = {
    {.kind = OPTICS_UI_ICON_CMD_RECT, .params = {4.0f, 4.0f, 16.0f, 16.0f}},
};

static void test_builtin_ids_resolve(void) {
    const optics_ui_icon_id ids[] = {
        OPTICS_UI_ICON_CHEVRON_UP,
        OPTICS_UI_ICON_CHEVRON_DOWN,
        OPTICS_UI_ICON_CHEVRON_LEFT,
        OPTICS_UI_ICON_CHEVRON_RIGHT,
    };
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        const optics_ui_icon_desc *d = optics_ui_icon_info(nullptr, ids[i]);
        OT_EXPECT(d != nullptr);
        OT_EXPECT(d->count == 3u);
        OT_EXPECT(d->cmds != nullptr);
        OT_EXPECT(d->paint == OPTICS_UI_ICON_PAINT_STROKE);
    }
}

static void test_invalid_id_is_null(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    OT_EXPECT(optics_ui_icon_info(ui, OPTICS_UI_ICON_NONE) == nullptr);
    OT_EXPECT(optics_ui_icon_info(ui, OPTICS_UI_ICON_BUILTIN_COUNT) == nullptr);
    OT_EXPECT(optics_ui_icon_info(ui, (optics_ui_icon_id)0x99990001u) == nullptr);

    optics_ui_release(ui);
}

static void test_runtime_registered_id_resolves_and_unregisters(void) {
    optics_ui *ui_a = nullptr;
    optics_ui *ui_b = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui_a) == OPTICS_GFX_OK);
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui_b) == OPTICS_GFX_OK);

    optics_ui_icon_id id = optics_ui_icon_register(ui_a, &(optics_ui_icon_desc){
                                                             .cmds = k_box_cmds,
                                                             .count = 1u,
                                                             .paint = OPTICS_UI_ICON_PAINT_FILL,
                                                         });
    OT_EXPECT(id != OPTICS_UI_ICON_NONE);

    const optics_ui_icon_desc *d = optics_ui_icon_info(ui_a, id);
    OT_EXPECT(d != nullptr);
    OT_EXPECT(d->count == 1u);
    OT_EXPECT(d->paint == OPTICS_UI_ICON_PAINT_FILL);
    /* Strictly per-context: ui_b does not see ui_a's icon. */
    OT_EXPECT(optics_ui_icon_info(ui_b, id) == nullptr);
    OT_EXPECT(optics_ui_icon_info(nullptr, id) == nullptr);

    /* Unregister invalidates the old generational handle; re-registering gets a new handle. */
    optics_ui_icon_unregister(ui_a, id);
    OT_EXPECT(optics_ui_icon_info(ui_a, id) == nullptr);

    optics_ui_icon_id id2 = optics_ui_icon_register(ui_a, &(optics_ui_icon_desc){
                                                              .cmds = k_box_cmds,
                                                              .count = 1u,
                                                              .paint = OPTICS_UI_ICON_PAINT_STROKE,
                                                          });
    OT_EXPECT(id2 != OPTICS_UI_ICON_NONE);
    OT_EXPECT(id2 != id);
    OT_EXPECT(optics_ui_icon_info(ui_a, id) == nullptr);
    OT_EXPECT(optics_ui_icon_info(ui_a, id2) != nullptr);

    optics_ui_release(ui_b);
    optics_ui_release(ui_a);
}

int main(void) {
    test_builtin_ids_resolve();
    test_invalid_id_is_null();
    test_runtime_registered_id_resolves_and_unregisters();
    return OT_REPORT();
}
