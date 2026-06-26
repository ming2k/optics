/* a11y_util.c — pure helpers for the AT-SPI bridge. See a11y_util.h. */

#include "a11y_util.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

bool iris_a11y__supports_action(lens_role r) {
    switch (r) {
    case LENS_ROLE_BUTTON:
    case LENS_ROLE_CHECKBOX:
    case LENS_ROLE_RADIO:
    case LENS_ROLE_DISCLOSURE:
    case LENS_ROLE_MENU:
        return true;
    case LENS_ROLE_LABEL:
    case LENS_ROLE_GROUP:
    case LENS_ROLE_SCROLLAREA:
    case LENS_ROLE_TEXTFIELD:
    case LENS_ROLE_TEXTAREA:
    case LENS_ROLE_SLIDER:
    case LENS_ROLE_NONE:
    default:
        return false;
    }
}

bool iris_a11y__supports_text(lens_role r) {
    return r == LENS_ROLE_TEXTFIELD || r == LENS_ROLE_TEXTAREA;
}

bool iris_a11y__supports_value(lens_role r) {
    return r == LENS_ROLE_SLIDER;
}

const char *iris_a11y__action_name(lens_role r) {
    switch (r) {
    case LENS_ROLE_CHECKBOX:
    case LENS_ROLE_RADIO:
    case LENS_ROLE_DISCLOSURE:
        return "toggle";
    case LENS_ROLE_BUTTON:
    case LENS_ROLE_MENU:
        return "click";
    default:
        return NULL;
    }
}

double iris_a11y__parse_value(const char *s) {
    if (!s || !*s)
        return 0.0;
    /* strtod skips leading whitespace and parses as much as is valid; a
     * non-numeric readout (e.g. a label accidentally queried) yields 0.0
     * because end == s after the parse. */
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s)
        return 0.0;
    return v;
}

int iris_a11y__char_count_utf8(const char *s) {
    if (!s)
        return 0;
    /* AT-SPI Text offsets count Unicode code points, not bytes or UTF-16
     * units. A UTF-8 leading byte is any byte not in the 10xxxxxx
     * continuation range, so counting those gives the code-point count. */
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if ((*p & 0xC0) != 0x80)
            n++;
    return n;
}
