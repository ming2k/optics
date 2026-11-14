/* test_uri.c — iris_file_uri_to_path decoding contract.
 *
 * Headless: pure string transformation, no platform or portal deps.
 */

#include "optics_test.h"
#include <iris/file_dialog.h>
#include <stdio.h>
#include <string.h>

static int check_decodes(const char *uri, const char *want) {
    char buf[512];
    int rc = iris_file_uri_to_path(uri, buf, sizeof buf);
    if (rc != 0) {
        fprintf(stderr, "uri %s: unexpected rc %d\n", uri, rc);
        return 0;
    }
    if (strcmp(buf, want) != 0) {
        fprintf(stderr, "uri %s: got \"%s\", want \"%s\"\n", uri, buf, want);
        return 0;
    }
    return 1;
}

int main(void) {
    char buf[512];

    /* Plain paths pass through. */
    OT_EXPECT(check_decodes("file:///home/user/foo.txt", "/home/user/foo.txt"));
    OT_EXPECT(check_decodes("file:///", "/"));
    OT_EXPECT(check_decodes("file:///tmp", "/tmp"));

    /* Percent escapes decode. */
    OT_EXPECT(check_decodes("file:///home/my%20docs/a.png", "/home/my docs/a.png"));
    OT_EXPECT(check_decodes("file:///a%2Fb", "/a/b")); /* %2F is a real slash after decode */
    OT_EXPECT(check_decodes("file:///caf%C3%A9.png", "/café.png"));
    OT_EXPECT(check_decodes("file:///100%25.png", "/100%.png"));

    /* '+' is NOT a space in file URIs (that is the query-string convention);
     * it must pass through literally. */
    OT_EXPECT(check_decodes("file:///a+b.txt", "/a+b.txt"));

    /* Non-empty authority is refused (a remote resource, not a local path). */
    OT_EXPECT(iris_file_uri_to_path("file://server/share/x", buf, sizeof buf) == -2);

    /* Malformed escapes are refused, not repaired. */
    OT_EXPECT(iris_file_uri_to_path("file:///a%2", buf, sizeof buf) == -3);
    OT_EXPECT(iris_file_uri_to_path("file:///a%zz", buf, sizeof buf) == -3);
    OT_EXPECT(iris_file_uri_to_path("file:///a%", buf, sizeof buf) == -3);

    /* Wrong scheme / missing path are refused. */
    OT_EXPECT(iris_file_uri_to_path("http://example.com/x", buf, sizeof buf) == -1);
    OT_EXPECT(iris_file_uri_to_path("file://host", buf, sizeof buf) == -1);

    /* Null / zero-cap arguments. */
    OT_EXPECT(iris_file_uri_to_path(NULL, buf, sizeof buf) == -1);
    OT_EXPECT(iris_file_uri_to_path("file:///x", NULL, 8) == -1);
    OT_EXPECT(iris_file_uri_to_path("file:///x", buf, 0) == -1);

    /* Too-small output reports -4 and leaves a NUL-terminated prefix
     * (shorter than the decoded path, never unterminated). */
    OT_EXPECT(iris_file_uri_to_path("file:///abcdefgh", buf, 5) == -4);
    OT_EXPECT(strlen(buf) == 4); /* cap-1 usable bytes */
    OT_EXPECT(buf[4] == '\0');

    /* Raw UTF-8 bytes (not percent-encoded) pass through verbatim. */
    OT_EXPECT(check_decodes("file:///café.png", "/café.png"));

    return OT_REPORT();
}
