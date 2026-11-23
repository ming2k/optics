/* test_a11y_util.c — pure logic of the AT-SPI bridge (a11y_util.c).
 *
 * Headless: no sd-bus, no display. These predicates decide which AT-SPI
 * interfaces (Action / Text / Value) each ui role exposes, plus the
 * slider-readout parser and the UTF-8 code-point counter the Text interface
 * reports offsets in.
 *
 * a11y_util.c is compiled directly into this test (rather than linked from
 * liboptics_window) because the helpers are internal / hidden-visibility.
 */

#include "a11y_util.h"
#include "optics_test.h"

int main(void) {
    /* --- Action support --- */
    OT_EXPECT(optics_window_a11y__supports_action(OPTICS_WINDOW_ROLE_BUTTON));
    OT_EXPECT(optics_window_a11y__supports_action(OPTICS_WINDOW_ROLE_CHECKBOX));
    OT_EXPECT(optics_window_a11y__supports_action(OPTICS_WINDOW_ROLE_RADIO));
    OT_EXPECT(optics_window_a11y__supports_action(OPTICS_WINDOW_ROLE_DISCLOSURE));
    OT_EXPECT(!optics_window_a11y__supports_action(OPTICS_WINDOW_ROLE_LABEL));
    OT_EXPECT(!optics_window_a11y__supports_action(OPTICS_WINDOW_ROLE_SLIDER));
    OT_EXPECT(!optics_window_a11y__supports_action(OPTICS_WINDOW_ROLE_TEXTFIELD));
    OT_EXPECT(!optics_window_a11y__supports_action(OPTICS_WINDOW_ROLE_NONE));

    /* --- Text support --- */
    OT_EXPECT(optics_window_a11y__supports_text(OPTICS_WINDOW_ROLE_TEXTFIELD));
    OT_EXPECT(optics_window_a11y__supports_text(OPTICS_WINDOW_ROLE_TEXTAREA));
    OT_EXPECT(!optics_window_a11y__supports_text(OPTICS_WINDOW_ROLE_BUTTON));
    OT_EXPECT(!optics_window_a11y__supports_text(OPTICS_WINDOW_ROLE_LABEL));

    /* --- Value support --- */
    OT_EXPECT(optics_window_a11y__supports_value(OPTICS_WINDOW_ROLE_SLIDER));
    OT_EXPECT(!optics_window_a11y__supports_value(OPTICS_WINDOW_ROLE_BUTTON));
    OT_EXPECT(!optics_window_a11y__supports_value(OPTICS_WINDOW_ROLE_CHECKBOX));

    /* --- Action names --- */
    OT_EXPECT_STR_EQ(optics_window_a11y__action_name(OPTICS_WINDOW_ROLE_BUTTON), "click");
    OT_EXPECT_STR_EQ(optics_window_a11y__action_name(OPTICS_WINDOW_ROLE_MENU), "click");
    OT_EXPECT_STR_EQ(optics_window_a11y__action_name(OPTICS_WINDOW_ROLE_CHECKBOX), "click");
    OT_EXPECT_STR_EQ(optics_window_a11y__action_name(OPTICS_WINDOW_ROLE_RADIO), "click");
    OT_EXPECT_STR_EQ(optics_window_a11y__action_name(OPTICS_WINDOW_ROLE_DISCLOSURE), "click");
    OT_EXPECT(optics_window_a11y__action_name(OPTICS_WINDOW_ROLE_LABEL) == nullptr);

    /* --- Value parser --- */
    OT_EXPECT(optics_window_a11y__parse_value("1.5") == 1.5);
    OT_EXPECT(optics_window_a11y__parse_value("42") == 42.0);
    OT_EXPECT(optics_window_a11y__parse_value("-0.25") == -0.25);
    OT_EXPECT(optics_window_a11y__parse_value("  7  ") == 7.0); /* strtod skips ws */
    OT_EXPECT(optics_window_a11y__parse_value("") == 0.0);
    OT_EXPECT(optics_window_a11y__parse_value(nullptr) == 0.0);
    OT_EXPECT(optics_window_a11y__parse_value("not a number") == 0.0);

    /* --- UTF-8 code-point count (AT-SPI Text offsets) --- */
    OT_EXPECT(optics_window_a11y__char_count_utf8(nullptr) == 0);
    OT_EXPECT(optics_window_a11y__char_count_utf8("") == 0);
    OT_EXPECT(optics_window_a11y__char_count_utf8("abc") == 3);
    /* "héllo" — é is 2 bytes in UTF-8, still 1 code point → 5 chars total. */
    OT_EXPECT(optics_window_a11y__char_count_utf8("h\xc3\xa9llo") == 5);
    /* "日本" — 3 bytes each, 2 code points. */
    OT_EXPECT(optics_window_a11y__char_count_utf8("\xe6\x97\xa5\xe6\x9c\xac") == 2);

    /* --- New roles (ADR-0035 extension) land in the interface map --- */
    OT_EXPECT(optics_window_a11y__supports_action(OPTICS_WINDOW_ROLE_MENUITEM));
    OT_EXPECT(optics_window_a11y__supports_action(OPTICS_WINDOW_ROLE_LINK));
    OT_EXPECT(!optics_window_a11y__supports_action(OPTICS_WINDOW_ROLE_PROGRESS));
    OT_EXPECT(!optics_window_a11y__supports_action(OPTICS_WINDOW_ROLE_TABLE));
    OT_EXPECT(!optics_window_a11y__supports_action(OPTICS_WINDOW_ROLE_ROW));
    OT_EXPECT(optics_window_a11y__supports_value(OPTICS_WINDOW_ROLE_PROGRESS));
    OT_EXPECT(!optics_window_a11y__supports_value(OPTICS_WINDOW_ROLE_ROW));
    OT_EXPECT_STR_EQ(optics_window_a11y__action_name(OPTICS_WINDOW_ROLE_MENUITEM), "click");
    OT_EXPECT_STR_EQ(optics_window_a11y__action_name(OPTICS_WINDOW_ROLE_LINK), "click");

    /* --- Role mapping: exact AT-SPI wire values (atspi-constants.h).
     * Regression cover for the audit finding that these numbers drifted
     * from the protocol (clients compare them numerically). --- */
    OT_EXPECT(optics_window_a11y__map_role(OPTICS_WINDOW_ROLE_BUTTON) == 43);  /* PUSH_BUTTON   */
    OT_EXPECT(optics_window_a11y__map_role(OPTICS_WINDOW_ROLE_CHECKBOX) == 7); /* CHECK_BOX     */
    OT_EXPECT(optics_window_a11y__map_role(OPTICS_WINDOW_ROLE_RADIO) == 44);   /* RADIO_BUTTON  */
    OT_EXPECT(optics_window_a11y__map_role(OPTICS_WINDOW_ROLE_SLIDER) == 51);  /* SLIDER        */
    OT_EXPECT(optics_window_a11y__map_role(OPTICS_WINDOW_ROLE_LABEL) == 29);   /* LABEL         */
    OT_EXPECT(optics_window_a11y__map_role(OPTICS_WINDOW_ROLE_GROUP) == 39);   /* PANEL         */
    OT_EXPECT(optics_window_a11y__map_role(OPTICS_WINDOW_ROLE_SCROLLAREA) == 49); /* SCROLL_PANE */
    OT_EXPECT(optics_window_a11y__map_role(OPTICS_WINDOW_ROLE_TEXTFIELD) == 79);  /* ENTRY       */
    OT_EXPECT(optics_window_a11y__map_role(OPTICS_WINDOW_ROLE_TEXTAREA) == 61);   /* TEXT        */
    OT_EXPECT(optics_window_a11y__map_role(OPTICS_WINDOW_ROLE_DISCLOSURE) ==
              62);                                                              /* TOGGLE_BUTTON */
    OT_EXPECT(optics_window_a11y__map_role(OPTICS_WINDOW_ROLE_MENU) == 33);     /* MENU          */
    OT_EXPECT(optics_window_a11y__map_role(OPTICS_WINDOW_ROLE_DIALOG) == 16);   /* DIALOG        */
    OT_EXPECT(optics_window_a11y__map_role(OPTICS_WINDOW_ROLE_PROGRESS) == 42); /* PROGRESS_BAR  */
    OT_EXPECT(optics_window_a11y__map_role(OPTICS_WINDOW_ROLE_TABLE) == 55);    /* TABLE         */
    OT_EXPECT(optics_window_a11y__map_role(OPTICS_WINDOW_ROLE_ROW) == 90);      /* TABLE_ROW     */
    OT_EXPECT(optics_window_a11y__map_role(OPTICS_WINDOW_ROLE_MENUITEM) == 35); /* MENU_ITEM     */
    OT_EXPECT(optics_window_a11y__map_role(OPTICS_WINDOW_ROLE_LINK) == 88);     /* LINK          */
    OT_EXPECT(optics_window_a11y__map_role(OPTICS_WINDOW_ROLE_NONE) == 67);     /* UNKNOWN       */
    OT_EXPECT_STR_EQ(optics_window_a11y__role_name(OPTICS_WINDOW_ATSPI_ROLE_PROGRESS_BAR),
                     "progress bar");
    OT_EXPECT_STR_EQ(optics_window_a11y__role_name(OPTICS_WINDOW_ATSPI_ROLE_TABLE_ROW),
                     "table row");

    /* --- State bits: exact AtspiStateType positions --- */
    uint32_t lo = 0, hi = 0;
    optics_window_a11y__state_bits(0, &lo, &hi);
    /* default: FOCUSABLE(11) | ENABLED(8) | SENSITIVE(24) | SHOWING(25) */
    OT_EXPECT(lo == ((1u << 11) | (1u << 8) | (1u << 24) | (1u << 25)));
    OT_EXPECT(hi == 0);
    optics_window_a11y__state_bits(OPTICS_WINDOW_A11Y_FOCUSED | OPTICS_WINDOW_A11Y_CHECKED |
                                       OPTICS_WINDOW_A11Y_EXPANDED | OPTICS_WINDOW_A11Y_SELECTED,
                                   &lo, &hi);
    /* + FOCUSED(12) | CHECKED(4) | EXPANDED(10) | SELECTED(23) */
    OT_EXPECT(lo == ((1u << 11) | (1u << 8) | (1u << 24) | (1u << 25) | (1u << 12) | (1u << 4) |
                     (1u << 10) | (1u << 23)));
    OT_EXPECT(hi == 0);
    optics_window_a11y__state_bits(OPTICS_WINDOW_A11Y_DISABLED, &lo, &hi);
    OT_EXPECT(lo == (1u << 25)); /* SHOWING only */
    OT_EXPECT(hi == 0);

    /* --- node_fill: deep copies + sibling index --- */
    optics_window_a11y__node cur[8];
    memset(cur, 0, sizeof cur);
    optics_window_semantics sem = {
        .role = OPTICS_WINDOW_ROLE_BUTTON, .name = "OK", .value = nullptr, .flags = 0};
    optics_window_a11y__node_fill(&cur[0], &sem, 100, 0, cur, 0);
    OT_EXPECT_STR_EQ(cur[0].name, "OK");
    OT_EXPECT(cur[0].value[0] == '\0');
    OT_EXPECT(cur[0].index == 0);
    /* name must be a copy, not the arena pointer */
    OT_EXPECT(cur[0].name != sem.name);
    sem.name = "Cancel";
    optics_window_a11y__node_fill(&cur[1], &sem, 101, 0, cur, 1);
    OT_EXPECT(cur[1].index == 1);        /* second child of parent 0 */
    OT_EXPECT_STR_EQ(cur[0].name, "OK"); /* first node's copy survives */
    /* a different parent restarts the index */
    optics_window_a11y__node_fill(&cur[2], &sem, 102, 100, cur, 2);
    OT_EXPECT(cur[2].index == 0);

    /* --- diff: additions, removals, property and state changes --- */
    optics_window_a11y__event evs[32];

    /* frame 1: two buttons; frame 2: one renamed, one removed, one added */
    optics_window_a11y__node f1[4], f2[4];
    memset(f1, 0, sizeof f1);
    memset(f2, 0, sizeof f2);
    optics_window_semantics s1 = {
        .role = OPTICS_WINDOW_ROLE_BUTTON, .name = "One", .value = nullptr, .flags = 0};
    optics_window_semantics s2 = {.role = OPTICS_WINDOW_ROLE_CHECKBOX,
                                  .name = "Two",
                                  .value = nullptr,
                                  .flags = OPTICS_WINDOW_A11Y_CHECKED};
    optics_window_a11y__node_fill(&f1[0], &s1, 1, 0, f1, 0);
    optics_window_a11y__node_fill(&f1[1], &s2, 2, 0, f1, 1);
    size_t n = optics_window_a11y__diff(nullptr, 0, f1, 2, evs, 32);
    OT_EXPECT(n == 2); /* both added */
    OT_EXPECT(evs[0].kind == OPTICS_WINDOW_A11Y__EV_ADD && evs[0].id == 1 && evs[0].index == 0);
    OT_EXPECT(evs[1].kind == OPTICS_WINDOW_A11Y__EV_ADD && evs[1].id == 2 && evs[1].index == 1);

    /* frame 2: id 1 renamed, id 2 removed, id 3 added (unchecked, new name) */
    optics_window_semantics s1b = {
        .role = OPTICS_WINDOW_ROLE_BUTTON, .name = "One!", .value = nullptr, .flags = 0};
    optics_window_semantics s3 = {
        .role = OPTICS_WINDOW_ROLE_SLIDER, .name = "Vol", .value = "0.5", .flags = 0};
    optics_window_a11y__node_fill(&f2[0], &s1b, 1, 0, f2, 0);
    optics_window_a11y__node_fill(&f2[1], &s3, 3, 0, f2, 1);
    n = optics_window_a11y__diff(f1, 2, f2, 2, evs, 32);
    /* expect: REMOVE(2), ADD(3), NAME(1) — removals first */
    OT_EXPECT(n == 3);
    OT_EXPECT(evs[0].kind == OPTICS_WINDOW_A11Y__EV_REMOVE && evs[0].id == 2 && evs[0].index == 1);
    OT_EXPECT(evs[1].kind == OPTICS_WINDOW_A11Y__EV_ADD && evs[1].id == 3);
    OT_EXPECT(evs[2].kind == OPTICS_WINDOW_A11Y__EV_NAME && evs[2].id == 1);
    OT_EXPECT(evs[2].node && strcmp(evs[2].node->name, "One!") == 0);

    /* frame 3: slider value moves, checkbox state flips, role changes */
    optics_window_semantics s3b = {
        .role = OPTICS_WINDOW_ROLE_SLIDER, .name = "Vol", .value = "0.7", .flags = 0};
    optics_window_semantics s1c = {
        .role = OPTICS_WINDOW_ROLE_LINK, .name = "One!", .value = nullptr, .flags = 0};
    optics_window_a11y__node_fill(&f1[0], &s1c, 1, 0, f1, 0);
    optics_window_a11y__node_fill(&f1[1], &s3b, 3, 0, f1, 1);
    n = optics_window_a11y__diff(f2, 2, f1, 2, evs, 32);
    /* expect: VALUE(3), ROLE(1) — order: value after role? kinds emitted in
     * the fixed per-node order role, name, value, states; node order is the
     * cur walk order (id 1 first, then id 3). */
    OT_EXPECT(n == 2);
    OT_EXPECT(evs[0].kind == OPTICS_WINDOW_A11Y__EV_ROLE && evs[0].id == 1 &&
              evs[0].role == OPTICS_WINDOW_ROLE_LINK);
    OT_EXPECT(evs[1].kind == OPTICS_WINDOW_A11Y__EV_VALUE && evs[1].id == 3);

    /* state flip: checked -> unchecked on a stable id */
    optics_window_semantics c_on = {.role = OPTICS_WINDOW_ROLE_CHECKBOX,
                                    .name = "C",
                                    .value = nullptr,
                                    .flags = OPTICS_WINDOW_A11Y_CHECKED};
    optics_window_semantics c_off = {
        .role = OPTICS_WINDOW_ROLE_CHECKBOX, .name = "C", .value = nullptr, .flags = 0};
    memset(f1, 0, sizeof f1);
    memset(f2, 0, sizeof f2);
    optics_window_a11y__node_fill(&f1[0], &c_on, 5, 0, f1, 0);
    optics_window_a11y__node_fill(&f2[0], &c_off, 5, 0, f2, 0);
    n = optics_window_a11y__diff(f1, 1, f2, 1, evs, 32);
    OT_EXPECT(n == 1);
    OT_EXPECT(evs[0].kind == OPTICS_WINDOW_A11Y__EV_STATE_OFF &&
              evs[0].state == OPTICS_WINDOW_ATSPI_STATE_CHECKED);
    n = optics_window_a11y__diff(f2, 1, f1, 1, evs, 32);
    OT_EXPECT(n == 1);
    OT_EXPECT(evs[0].kind == OPTICS_WINDOW_A11Y__EV_STATE_ON &&
              evs[0].state == OPTICS_WINDOW_ATSPI_STATE_CHECKED);

    /* identical frames produce no events */
    n = optics_window_a11y__diff(f1, 1, f1, 1, evs, 32);
    OT_EXPECT(n == 0);

    /* --- text delta (ADR-0062 TextChanged) --- */
    optics_window_a11y__text_delta d;

    /* pure insert at the end: "" -> "hello" */
    OT_EXPECT(optics_window_a11y__text_delta_of("", "hello", &d));
    OT_EXPECT(d.offset == 0 && d.removed == 0 && d.inserted == 5);
    OT_EXPECT(d.inserted_bytes == 5 && memcmp(d.inserted_text, "hello", 5) == 0);

    /* insert in the middle: "ac" -> "abc" */
    OT_EXPECT(optics_window_a11y__text_delta_of("ac", "abc", &d));
    OT_EXPECT(d.offset == 1 && d.removed == 0 && d.inserted == 1);
    OT_EXPECT(d.inserted_bytes == 1 && d.inserted_text[0] == 'b');

    /* pure delete: "hello" -> "helo" */
    OT_EXPECT(optics_window_a11y__text_delta_of("hello", "helo", &d));
    OT_EXPECT(d.offset == 3 && d.removed == 1 && d.inserted == 0);
    OT_EXPECT(d.removed_bytes == 1 && d.removed_text[0] == 'l');

    /* replace: "cat" -> "cot" (both runs non-empty → delete+insert) */
    OT_EXPECT(optics_window_a11y__text_delta_of("cat", "cot", &d));
    OT_EXPECT(d.offset == 1 && d.removed == 1 && d.inserted == 1);
    OT_EXPECT(d.removed_text[0] == 'a' && d.inserted_text[0] == 'o');

    /* identical: no delta */
    OT_EXPECT(!optics_window_a11y__text_delta_of("same", "same", &d));
    OT_EXPECT(!optics_window_a11y__text_delta_of("", "", &d));

    /* multi-byte: code-point offsets, never a split sequence.
     * "héllo" -> "hélo": removes the second 'l' at cp offset 3. */
    OT_EXPECT(optics_window_a11y__text_delta_of("h\xc3\xa9llo", "h\xc3\xa9lo", &d));
    OT_EXPECT(d.offset == 3 && d.removed == 1 && d.inserted == 0);
    /* "é" -> "è": the shared C3 lead byte must NOT count as prefix;
     * the whole code point is the replaced run. */
    OT_EXPECT(optics_window_a11y__text_delta_of("\xc3\xa9", "\xc3\xa8", &d));
    OT_EXPECT(d.offset == 0 && d.removed == 1 && d.inserted == 1);
    OT_EXPECT(d.removed_bytes == 2 && d.inserted_bytes == 2);
    /* suffix alignment: "x日本" -> "y日本" — the 3-byte chars stay whole */
    OT_EXPECT(optics_window_a11y__text_delta_of("x\xe6\x97\xa5\xe6\x9c\xac",
                                                "y\xe6\x97\xa5\xe6\x9c\xac", &d));
    OT_EXPECT(d.offset == 0 && d.removed == 1 && d.inserted == 1);
    OT_EXPECT(d.removed_text[0] == 'x' && d.inserted_text[0] == 'y');

    /* NULL behaves as empty */
    OT_EXPECT(optics_window_a11y__text_delta_of(nullptr, "a", &d));
    OT_EXPECT(d.offset == 0 && d.inserted == 1);

    /* --- diff routes text-role value changes to EV_TEXT --- */
    memset(f1, 0, sizeof f1);
    memset(f2, 0, sizeof f2);
    optics_window_semantics t1 = {
        .role = OPTICS_WINDOW_ROLE_TEXTFIELD, .name = "F", .value = "hel", .flags = 0};
    optics_window_semantics t2 = {
        .role = OPTICS_WINDOW_ROLE_TEXTFIELD, .name = "F", .value = "hello", .flags = 0};
    optics_window_a11y__node_fill(&f1[0], &t1, 7, 0, f1, 0);
    optics_window_a11y__node_fill(&f2[0], &t2, 7, 0, f2, 0);
    n = optics_window_a11y__diff(f1, 1, f2, 1, evs, 32);
    OT_EXPECT(n == 1);
    OT_EXPECT(evs[0].kind == OPTICS_WINDOW_A11Y__EV_TEXT && evs[0].id == 7);
    OT_EXPECT(evs[0].text.offset == 3 && evs[0].text.removed == 0 && evs[0].text.inserted == 2);
    OT_EXPECT(evs[0].text.inserted_bytes == 2 && memcmp(evs[0].text.inserted_text, "lo", 2) == 0);

    return OT_REPORT();
}
