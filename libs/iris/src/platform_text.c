/* platform_text.c — shared UTF-8 helpers. See platform_text.h. */

#include "platform_text.h"

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
