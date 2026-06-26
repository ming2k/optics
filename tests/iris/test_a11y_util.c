/* test_a11y_util.c — pure logic of the AT-SPI bridge (a11y_util.c).
 *
 * Headless: no sd-bus, no display. These predicates decide which AT-SPI
 * interfaces (Action / Text / Value) each lens role exposes, plus the
 * slider-readout parser and the UTF-8 code-point counter the Text interface
 * reports offsets in.
 *
 * a11y_util.c is compiled directly into this test (rather than linked from
 * libiris) because the helpers are internal / hidden-visibility.
 */

#include "a11y_util.h"
#include "test_helpers.h"

int main(void) {
    /* --- Action support --- */
    CHECK(iris_a11y__supports_action(LENS_ROLE_BUTTON));
    CHECK(iris_a11y__supports_action(LENS_ROLE_CHECKBOX));
    CHECK(iris_a11y__supports_action(LENS_ROLE_RADIO));
    CHECK(iris_a11y__supports_action(LENS_ROLE_DISCLOSURE));
    CHECK(!iris_a11y__supports_action(LENS_ROLE_LABEL));
    CHECK(!iris_a11y__supports_action(LENS_ROLE_SLIDER));
    CHECK(!iris_a11y__supports_action(LENS_ROLE_TEXTFIELD));
    CHECK(!iris_a11y__supports_action(LENS_ROLE_NONE));

    /* --- Text support --- */
    CHECK(iris_a11y__supports_text(LENS_ROLE_TEXTFIELD));
    CHECK(iris_a11y__supports_text(LENS_ROLE_TEXTAREA));
    CHECK(!iris_a11y__supports_text(LENS_ROLE_BUTTON));
    CHECK(!iris_a11y__supports_text(LENS_ROLE_LABEL));

    /* --- Value support --- */
    CHECK(iris_a11y__supports_value(LENS_ROLE_SLIDER));
    CHECK(!iris_a11y__supports_value(LENS_ROLE_BUTTON));
    CHECK(!iris_a11y__supports_value(LENS_ROLE_CHECKBOX));

    /* --- Action names --- */
    CHECK_STR_EQ(iris_a11y__action_name(LENS_ROLE_BUTTON), "click");
    CHECK_STR_EQ(iris_a11y__action_name(LENS_ROLE_MENU), "click");
    CHECK_STR_EQ(iris_a11y__action_name(LENS_ROLE_CHECKBOX), "toggle");
    CHECK_STR_EQ(iris_a11y__action_name(LENS_ROLE_RADIO), "toggle");
    CHECK_STR_EQ(iris_a11y__action_name(LENS_ROLE_DISCLOSURE), "toggle");
    CHECK(iris_a11y__action_name(LENS_ROLE_LABEL) == NULL);

    /* --- Value parser --- */
    CHECK(iris_a11y__parse_value("1.5") == 1.5);
    CHECK(iris_a11y__parse_value("42") == 42.0);
    CHECK(iris_a11y__parse_value("-0.25") == -0.25);
    CHECK(iris_a11y__parse_value("  7  ") == 7.0); /* strtod skips ws */
    CHECK(iris_a11y__parse_value("") == 0.0);
    CHECK(iris_a11y__parse_value(NULL) == 0.0);
    CHECK(iris_a11y__parse_value("not a number") == 0.0);

    /* --- UTF-8 code-point count (AT-SPI Text offsets) --- */
    CHECK(iris_a11y__char_count_utf8(NULL) == 0);
    CHECK(iris_a11y__char_count_utf8("") == 0);
    CHECK(iris_a11y__char_count_utf8("abc") == 3);
    /* "héllo" — é is 2 bytes in UTF-8, still 1 code point → 5 chars total. */
    CHECK(iris_a11y__char_count_utf8("h\xc3\xa9llo") == 5);
    /* "日本" — 3 bytes each, 2 code points. */
    CHECK(iris_a11y__char_count_utf8("\xe6\x97\xa5\xe6\x9c\xac") == 2);

    return TEST_REPORT();
}
