/* file_dialog_portal.c — file picker via xdg-desktop-portal.
 *
 * Strategy: shell out to gdbus to call org.freedesktop.portal.FileChooser.
 * This is the same path modern GTK4/Qt6 take when running inside a flatpak
 * or a Wayland session — the user sees their own DE's native picker, and
 * we never need filesystem UI of our own.
 *
 * The call blocks until the user confirms or cancels. The result is a file
 * URI written to the caller's buffer.
 *
 * Limitations of this minimal version:
 *   - No filters / mime types yet (passes an empty options dict).
 *   - Single file only.
 *   - The first URI in the response is returned; extras are dropped.
 *
 * gdbus call shape (the loop in main loops over the result URIs):
 *
 *   gdbus call --session \
 *     --dest org.freedesktop.portal.Desktop \
 *     --object-path /org/freedesktop/portal/desktop \
 *     --method org.freedesktop.portal.FileChooser.OpenFile \
 *              'x' 'Title' {}
 *
 * which returns "('x', {'uris': ['file:///...']})'. We extract the first
 * 'file://...' substring.
 */

#include <iris/file_dialog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

IRIS_API int iris_pick_file(const iris_file_dialog_opts *opts, char *out_path, size_t out_cap) {
    if (!out_path || out_cap == 0)
        return 1;

    const char *title = (opts && opts->title) ? opts->title : "Open File";

    /* Escape single quotes in title for the shell. We pass the title
     * directly because portal accepts UTF-8 and rejects anything weird. */
    char cmd[512];
    int n = snprintf(cmd, sizeof cmd,
                     "gdbus call --session "
                     "--dest org.freedesktop.portal.Desktop "
                     "--object-path /org/freedesktop/portal/desktop "
                     "--method org.freedesktop.portal.FileChooser.OpenFile "
                     "'iris' '%s' {} 2>/dev/null",
                     title);
    if (n < 0 || (size_t)n >= sizeof cmd)
        return 2;

    FILE *fp = popen(cmd, "r");
    if (!fp)
        return 3;

    char buf[2048] = {0};
    size_t got = fread(buf, 1, sizeof buf - 1, fp);
    int rc = pclose(fp);
    if (rc != 0 || got == 0)
        return 4;

    /* Find the first 'file://...' substring. */
    const char *p = strstr(buf, "file://");
    if (!p)
        return 5;

    /* Copy until the next single quote or end of string. */
    size_t i = 0;
    while (p[i] && p[i] != '\'' && i + 1 < out_cap) {
        out_path[i] = p[i];
        i++;
    }
    out_path[i] = '\0';
    return 0;
}
