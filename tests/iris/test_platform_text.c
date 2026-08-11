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

#include <lens/lens.h>
#include <string.h>

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

    /* --- lens_input staging sizes (the buffers the backends clip into) ---
     * text_utf8 grew to 256 for full-sentence IME conversion and preedit to
     * LENS_PREEDIT_MAX=256; the Wayland accumulator mirrors these sizes and
     * static_asserts against them, so pin the lens side here. */
    CHECK(sizeof ((lens_input *)0)->text_utf8 == 256);
    CHECK(sizeof ((lens_input *)0)->preedit_utf8 == 256);
    CHECK(LENS_PREEDIT_MAX == 256);

    /* A full-sentence IME commit staged at the new size: 300 bytes into a
     * 256 buffer keeps 255 (NUL takes the last byte), boundary-clean. */
    char big[sizeof ((lens_input *)0)->text_utf8];
    char src[300];
    memset(src, 'a', sizeof src - 1);
    src[sizeof src - 1] = '\0';
    big[0] = '\0';
    CHECK(iris_utf8_append(big, sizeof big, src, sizeof src - 1) == sizeof big - 1);
    CHECK(strlen(big) == sizeof big - 1);
    /* …and with a multi-byte char straddling the edge: 252 × 'a' + "日本"
     * (6 bytes) = 258 bytes; room is 255, so 日 lands whole at 252–254 and
     * 本 is dropped whole — the cut stays on the code-point boundary. */
    big[0] = '\0';
    memset(src, 'a', 252);
    memcpy(src + 252, "\xe6\x97\xa5\xe6\x9c\xac", 6);
    src[258] = '\0';
    CHECK(iris_utf8_append(big, sizeof big, src, 258) == 255);
    CHECK(strlen(big) == 255);
    CHECK(memcmp(big + 252, "\xe6\x97\xa5", 3) == 0);

    /* --- text memento (report-only-on-change for IME surrounding text) --- */
    char *mem = NULL;
    size_t mem_len = 0;
    uint32_t mem_cursor = 0;

    /* first report: always due */
    CHECK(iris_text_memento_update(&mem, &mem_len, &mem_cursor, "hello", 5, 5) == true);
    CHECK(mem_len == 5 && mem_cursor == 5);
    CHECK(mem && memcmp(mem, "hello", 5) == 0);
    /* identical text + cursor: not due */
    CHECK(iris_text_memento_update(&mem, &mem_len, &mem_cursor, "hello", 5, 5) == false);
    /* cursor moved: due */
    CHECK(iris_text_memento_update(&mem, &mem_len, &mem_cursor, "hello", 5, 3) == true);
    CHECK(mem_cursor == 3);
    /* text changed, same cursor: due */
    CHECK(iris_text_memento_update(&mem, &mem_len, &mem_cursor, "hellp", 5, 3) == true);
    CHECK(memcmp(mem, "hellp", 5) == 0);
    /* length changed: due */
    CHECK(iris_text_memento_update(&mem, &mem_len, &mem_cursor, "hellp!", 6, 3) == true);
    CHECK(mem_len == 6);
    /* empty text is a valid report */
    CHECK(iris_text_memento_update(&mem, &mem_len, &mem_cursor, "", 0, 0) == true);
    CHECK(mem_len == 0 && mem_cursor == 0);
    CHECK(iris_text_memento_update(&mem, &mem_len, &mem_cursor, "", 0, 0) == false);
    /* clear forgets the copy: the next report is due even if identical */
    iris_text_memento_clear(&mem, &mem_len, &mem_cursor);
    CHECK(mem == NULL && mem_len == 0 && mem_cursor == 0);
    CHECK(iris_text_memento_update(&mem, &mem_len, &mem_cursor, "", 0, 0) == true);
    iris_text_memento_clear(&mem, &mem_len, &mem_cursor);

    return TEST_REPORT();
}
