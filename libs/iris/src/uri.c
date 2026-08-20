/* uri.c — file:// URI → local path conversion for the file-dialog results.
 *
 * iris_pick_file / iris_pick_save_path return host URIs ("file:///a%20b/c").
 * Hosts need filesystem paths; making every one hand-roll percent-decoding
 * (with its own take on '+' vs %20, over-long sequences, and the authority
 * component) is exactly the kind of per-consumer drift a toolkit API
 * should own. The decode is strict on malformed input — the URI came from
 * the portal, so anything malformed is a programming or protocol error
 * worth surfacing, not silently repairing.
 */

#include <iris/file_dialog.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static int hex_val(unsigned char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Write one byte, bounds-checked. Returns false when out is full. */
static bool push(char *out, size_t cap, size_t *n, char c) {
    if (*n + 1 >= cap) /* keep room for the NUL */
        return false;
    out[(*n)++] = c;
    return true;
}

IRIS_API int iris_file_uri_to_path(const char *uri, char *out_path, size_t out_cap) {
    if (!uri || !out_path || out_cap == 0)
        return -1;
    out_path[0] = '\0';

    static const char scheme[] = "file://";
    const size_t scheme_len = sizeof scheme - 1;
    if (strncmp(uri, scheme, scheme_len) != 0)
        return -1;
    const char *p = uri + scheme_len;

    /* Authority: only the empty authority (a local path) is convertible.
     * "file://host/share" names a remote resource — the caller needs a
     * network filesystem layer we do not provide, so refuse it rather
     * than return a wrong local path. */
    const char *slash = strchr(p, '/');
    if (slash != p) {
        if (!slash)
            return -1; /* file:// with neither authority nor path */
        return -2;     /* non-empty authority: not a local path */
    }

    size_t n = 0;
    for (; *p; p++) {
        if (*p == '%') {
            int hi = hex_val((unsigned char)p[1]);
            if (hi < 0)
                return -3; /* '%' not followed by two hex digits */
            int lo = hex_val((unsigned char)p[2]);
            if (lo < 0)
                return -3;
            if (!push(out_path, out_cap, &n, (char)((hi << 4) | lo)))
                return -4; /* out too small */
            p += 2;
        } else if ((unsigned char)*p < 0x80) {
            if (!push(out_path, out_cap, &n, *p))
                return -4;
        } else {
            /* UTF-8 passes through verbatim: URIs from the portal are
             * percent-encoded ASCII, but a host may hand us a raw-UTF-8
             * URI; forwarding the bytes keeps the conversion total. */
            if (!push(out_path, out_cap, &n, *p))
                return -4;
        }
    }
    out_path[n] = '\0';
    return 0;
}
