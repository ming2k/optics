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

#include "test_helpers.h"
#include <lens/icon.h>
#include <lens/lens.h>
#include <string.h>

static const char *k_svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
                           "<path d=\"M4 4h16v16H4z\" fill=\"currentColor\"/></svg>";

static void test_builtin_ids_resolve(void) {
    /* A spread across the table: first, a mid, and the last built-in. */
    const lens_icon_id ids[] = {
        LENS_ICON_X,
        (lens_icon_id)(LENS_ICON_COUNT / 2),
        (lens_icon_id)(LENS_ICON_COUNT - 1),
    };
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        uint8_t mode = 0xFF;
        const lens_icon_desc *d = lens_icon_info(ids[i], &mode);
        CHECK(d != NULL);
        CHECK(d->count > 0);
        CHECK(d->cmds != NULL);
        CHECK(mode <= 1); /* 0 = fill, 1 = stroke render style */
    }
}

static void test_invalid_id_is_null(void) {
    CHECK(lens_icon_info(LENS_ICON_INVALID, NULL) == NULL);
    CHECK(lens_icon_info((lens_icon_id)-1, NULL) == NULL);
    CHECK(lens_icon_info((lens_icon_id)(LENS_ICON_COUNT + 100000), NULL) == NULL);
}

static void test_mode_out_param_is_optional(void) {
    /* NULL mode pointer must not crash; both spellings resolve equally. */
    const lens_icon_desc *a = lens_icon_info(LENS_ICON_ZAP, NULL);
    const lens_icon_desc *b = lens_icon_info(LENS_ICON_ZAP, &(uint8_t){0});
    CHECK(a != NULL && a == b);
}

static void test_runtime_registered_id_resolves(void) {
    lens_icon_id id = lens_icon_register_svg(k_svg);
    CHECK(id != LENS_ICON_INVALID);
    CHECK((int32_t)id >= (int32_t)LENS_ICON_COUNT);

    uint8_t mode = 0xFF;
    const lens_icon_desc *d = lens_icon_info(id, &mode);
    CHECK(d != NULL);
    CHECK(d->count > 0);
    CHECK(mode == 0); /* runtime SVG icons report the fill style */
}

int main(void) {
    test_builtin_ids_resolve();
    test_invalid_id_is_null();
    test_mode_out_param_is_optional();
    test_runtime_registered_id_resolves();
    puts("icon_info: all checks passed");
    return 0;
}
