/*
 * test_icon_info.c — lens_icon_info() public read access to the icon table.
 *
 * Contract under test (icon.h):
 *   - built-in ids resolve to non-NULL descriptors whose cmd count > 0;
 *   - LENS_ICON_INVALID (and any negative id) resolves to NULL;
 *   - runtime SVG ids (>= LENS_ICON_COUNT) resolve through the registry;
 *   - the mode out-param is optional and, when given, receives the
 *     built-in table's render mode (0 fill / 1 stroke).
 */

#include "lens_test.h"
#include "optics_test.h"
#include <optics/ui.h>
#include <optics/ui/icon.h>
#include <string.h>

static const char *k_svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
                           "<path d=\"M4 4h16v16H4z\" fill=\"currentColor\"/></svg>";

static void test_builtin_ids_resolve(void) {
    /* A spread across the table: first, a mid, and the last built-in. */
    const optics_ui_icon_id ids[] = {
        OPTICS_UI_ICON_X,
        (optics_ui_icon_id)(OPTICS_UI_ICON_COUNT / 2),
        (optics_ui_icon_id)(OPTICS_UI_ICON_COUNT - 1),
    };
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        uint8_t mode = 0xFF;
        const optics_ui_icon_desc *d = optics_ui_icon_info(ids[i], &mode);
        OT_EXPECT(d != nullptr);
        OT_EXPECT(d->count > 0);
        OT_EXPECT(d->cmds != nullptr);
        OT_EXPECT(mode <= 1); /* 0 = fill, 1 = stroke render style */
    }
}

static void test_invalid_id_is_null(void) {
    OT_EXPECT(optics_ui_icon_info(OPTICS_UI_ICON_INVALID, nullptr) == nullptr);
    OT_EXPECT(optics_ui_icon_info((optics_ui_icon_id)-1, nullptr) == nullptr);
    OT_EXPECT(optics_ui_icon_info((optics_ui_icon_id)(OPTICS_UI_ICON_COUNT + 100000), nullptr) ==
              nullptr);
}

static void test_mode_out_param_is_optional(void) {
    /* NULL mode pointer must not crash; both spellings resolve equally. */
    const optics_ui_icon_desc *a = optics_ui_icon_info(OPTICS_UI_ICON_ZAP, nullptr);
    const optics_ui_icon_desc *b = optics_ui_icon_info(OPTICS_UI_ICON_ZAP, &(uint8_t){});
    OT_EXPECT(a != nullptr && a == b);
}

static void test_runtime_registered_id_resolves(void) {
    optics_ui_icon_id id = optics_ui_icon_register_svg(k_svg);
    OT_EXPECT(id != OPTICS_UI_ICON_INVALID);
    OT_EXPECT((int32_t)id >= (int32_t)OPTICS_UI_ICON_COUNT);

    uint8_t mode = 0xFF;
    const optics_ui_icon_desc *d = optics_ui_icon_info(id, &mode);
    OT_EXPECT(d != nullptr);
    OT_EXPECT(d->count > 0);
    OT_EXPECT(mode == 1 || mode == 0);
}

int main(void) {
    test_builtin_ids_resolve();
    test_invalid_id_is_null();
    test_mode_out_param_is_optional();
    test_runtime_registered_id_resolves();
    puts("icon_info: all checks passed");
    return 0;
}
