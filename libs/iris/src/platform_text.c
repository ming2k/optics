/* platform_text.c — shared UTF-8 helpers. See platform_text.h. */

#include "platform_text.h"

#include <stdlib.h>
#include <string.h>

size_t iris_utf8_floor_boundary(const char *s, size_t len, size_t cap) {
    if (len <= cap)
        return len;
    size_t n = cap;
    /* s[n] is a continuation byte: back off to the sequence start. */
    while (n > 0 && (s[n] & 0xC0) == 0x80)
        n--;
    return n;
}

size_t iris_utf8_append(char *dst, size_t cap, const char *src, size_t n) {
    size_t used = strlen(dst);
    if (used >= cap - 1)
        return 0;
    size_t room = cap - 1 - used;
    n = iris_utf8_floor_boundary(src, n, room);
    memcpy(dst + used, src, n);
    dst[used + n] = '\0';
    return n;
}

void iris_utf8_copy(char *dst, size_t cap, const char *src) {
    dst[0] = '\0';
    if (src)
        iris_utf8_append(dst, cap, src, strlen(src));
}

bool iris_text_memento_update(char **saved, size_t *saved_len, uint32_t *saved_cursor,
                              const char *text, size_t len, uint32_t cursor) {
    /* A NULL memento means "nothing reported yet" (fresh session, or
     * cleared on session end): the report is always due, empty or not. */
    if (*saved && *saved_len == len && *saved_cursor == cursor &&
        (len == 0 || memcmp(*saved, text, len) == 0))
        return false; /* identical to the last report: nothing to send */
    char *copy = realloc(*saved, len ? len : 1);
    if (!copy)
        return true; /* report anyway; retry the copy on the next change */
    if (len)
        memcpy(copy, text, len);
    *saved = copy;
    *saved_len = len;
    *saved_cursor = cursor;
    return true;
}

void iris_text_memento_clear(char **saved, size_t *saved_len, uint32_t *saved_cursor) {
    free(*saved);
    *saved = NULL;
    *saved_len = 0;
    *saved_cursor = 0;
}
