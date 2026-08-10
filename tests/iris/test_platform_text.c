/* test_platform_text.c — UTF-8 boundary-aware truncation (platform_text.c).
 *
 * Headless: these helpers decide how IME commits / preedits / clipboard
 * payloads are clipped into lens's fixed-size input buffers on every
 * backend, so the exact boundary behaviour is pinned down here.
 *
 * platform_text.c is compiled directly into this test (the helpers are
 * internal / hidden-visibility, so they are not exported from libiris.so).
 */

#include "platform_text.h"
#include "test_helpers.h"

int main(void) {
    /* --- floor_boundary --- */
    /* ASCII: the cap lands wherever it lands. */
    CHECK(iris_utf8_floor_boundary("hello", 5, 3) == 3);
    CHECK(iris_utf8_floor_boundary("hello", 5, 5) == 5);
    CHECK(iris_utf8_floor_boundary("hello", 5, 9) == 5); /* len <= cap wins */
    CHECK(iris_utf8_floor_boundary("hello", 5, 0) == 0);
    /* "héllo" = h C3 A9 l l o (6 bytes). Cap 2 would cut é in half → 1. */
    CHECK(iris_utf8_floor_boundary("h\xc3\xa9llo", 6, 2) == 1);
    CHECK(iris_utf8_floor_boundary("h\xc3\xa9llo", 6, 3) == 3); /* exact fit */
    /* "日本" = 6 bytes; cap 4 backs off to the 日 boundary at 3. */
    CHECK(iris_utf8_floor_boundary("\xe6\x97\xa5\xe6\x9c\xac", 6, 4) == 3);
    /* cap lands exactly on a continuation byte start edge: no backoff */
    CHECK(iris_utf8_floor_boundary("\xe6\x97\xa5\xe6\x9c\xac", 6, 3) == 3);

    /* --- append --- */
    char buf[8];

    /* appends within capacity */
    buf[0] = '\0';
    CHECK(iris_utf8_append(buf, sizeof buf, "abc", 3) == 3);
    CHECK_STR_EQ(buf, "abc");

    /* appends on a code-point boundary when full */
    CHECK(iris_utf8_append(buf, sizeof buf, "\xc3\xa9""zzw", 5) == 4); /* ézz, w cut */
    CHECK_STR_EQ(buf, "abc\xc3\xa9zz");

    /* no room left: appends nothing, buffer stays NUL-terminated */
    CHECK(iris_utf8_append(buf, sizeof buf, "x", 1) == 0);
    CHECK_STR_EQ(buf, "abc\xc3\xa9zz");

    /* oversized single append is boundary-truncated, not dropped */
    buf[0] = '\0';
    CHECK(iris_utf8_append(buf, 5, "a\xc3\xa9""bcd", 6) == 4); /* room = 4: "aéb" */
    CHECK_STR_EQ(buf, "a\xc3\xa9""b");

    /* --- copy --- */
    iris_utf8_copy(buf, sizeof buf, "h\xc3\xa9llo");
    CHECK_STR_EQ(buf, "h\xc3\xa9llo");
    iris_utf8_copy(buf, 4, "h\xc3\xa9llo"); /* cap-1 = 3: "hé" exactly */
    CHECK_STR_EQ(buf, "h\xc3\xa9");
    iris_utf8_copy(buf, 3, "h\xc3\xa9llo"); /* cap-1 = 2: é splits → "h" */
    CHECK_STR_EQ(buf, "h");
    iris_utf8_copy(buf, sizeof buf, NULL);
    CHECK_STR_EQ(buf, "");

    return TEST_REPORT();
}
