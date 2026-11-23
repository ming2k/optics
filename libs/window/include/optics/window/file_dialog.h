/*
 * window/file_dialog.h — file picker via the host desktop.
 *
 * On Linux, talks directly to xdg-desktop-portal's FileChooser interface
 * over the user D-Bus. This is the same path modern GTK/Qt take — the user
 * sees their own DE's native picker, and the calling app does not need
 * filesystem UI of its own.
 *
 * Blocking: the call runs the portal's modal loop and returns when the
 * user confirms or cancels. Returns 0 on success, non-zero otherwise.
 */
#ifndef OPTICS_WINDOW_FILE_DIALOG_H
#define OPTICS_WINDOW_FILE_DIALOG_H

#include <optics/window/loop.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A name + glob pair such as {"Text", "*.txt"}. `pattern` may be a single
 * glob ("*.txt") or a semicolon-separated list ("*.txt;*.md"). A NULL or
 * empty pattern means "any file". */
typedef struct optics_window_file_filter {
    const char *name;    /* human label, UTF-8 (optional)            */
    const char *pattern; /* glob expression, UTF-8 (optional)        */
} optics_window_file_filter;

typedef struct optics_window_file_dialog_opts {
    const char *title;                        /* dialog title (UTF-8, optional)        */
    const char *initial_uri;                  /* file:// URI to start at (optional)    */
    const optics_window_file_filter *filters; /* NULL-terminated array (opt)   */
    bool multiple_selection;                  /* allow more than one (open mode only)  */
    bool directory_only;                      /* restrict to directories               */
    const char *default_name;                 /* suggested filename for save (opt)     */
    char *out_path;                           /* destination buffer for URI(s)         */
    size_t out_cap;                           /* capacity of out_path in bytes         */
    size_t *out_bytes_used;                   /* bytes written / needed (pick_files)   */
} optics_window_file_dialog_opts;

/* Result codes shared by every picker. Non-zero means "no selection".
 * The specific negative values let a host distinguish "the user changed
 * their mind" from "your buffer was too small" — which previously
 * behaved differently per backend (portal reported a cancel, Win32
 * silently truncated, Cocoa failed) with none of it documented. */
#define OPTICS_WINDOW_PICK_CANCELLED (-1)   /* user dismissed the dialog              */
#define OPTICS_WINDOW_PICK_UNAVAILABLE (-2) /* no dialog service on this platform     */
#define OPTICS_WINDOW_PICK_TRUNCATED (-3)   /* out buffer too small: retry with more  */

/* Open a single-file picker. On success, writes a NUL-terminated UTF-8
 * file URI (e.g. "file:///home/user/foo.txt") into opts->out_path and
 * returns 0. `multiple_selection` is ignored here (it belongs to
 * optics_window_pick_files); passing it does not widen this dialog on any backend.
 * Returns OPTICS_WINDOW_PICK_CANCELLED / OPTICS_WINDOW_PICK_UNAVAILABLE /
 * OPTICS_WINDOW_PICK_TRUNCATED. */
OPTICS_WINDOW_API int optics_window_pick_file(const optics_window_file_dialog_opts *opts);

/* Open a single-folder picker. The return convention and URI encoding match
 * optics_window_pick_file. */
OPTICS_WINDOW_API int optics_window_pick_folder(const optics_window_file_dialog_opts *opts);

/* Save dialog: asks the user for a destination path. `default_name` is the
 * suggested filename (UTF-8, optional); on success a file:// URI is written
 * to opts->out_path. The file is not created — opening it for write is the
 * host's responsibility. */
OPTICS_WINDOW_API int optics_window_pick_save_path(const optics_window_file_dialog_opts *opts);

/* Multi-selection picker. On success, writes NUL-separated UTF-8 file URIs
 * into opts->out_path (the list is terminated by an empty URI: one extra NUL
 * after the last entry) and returns the number of URIs (>= 1).
 *
 * Buffer-too-small is OPTICS_WINDOW_PICK_TRUNCATED on EVERY backend: the result is
 * never a silent prefix and never indistinguishable from a cancel. The
 * complete selection is not recoverable after truncation — reopen the
 * dialog with a larger buffer. *opts->out_bytes_used (optional) receives the
 * total bytes written excluding the trailing list terminator NUL; on
 * OPTICS_WINDOW_PICK_TRUNCATED it receives the number of bytes the FULL selection
 * would have required, so the retry size is directly computable. */
OPTICS_WINDOW_API int optics_window_pick_files(const optics_window_file_dialog_opts *opts);

/* Convert a `file://` URI (as returned by the pickers above) into a local
 * filesystem path, percent-decoding %XX escapes. Returns 0 on success.
 * Returns -1 for invalid arguments or a URI that is not `file://`,
 * -2 for a non-empty authority component (`file://host/...` is a remote
 * resource, not a local path), -3 for a malformed escape (a '%' not
 * followed by two hex digits), and -4 when out_cap is too small for the
 * decoded path. Non-ASCII bytes pass through verbatim: the portal emits
 * percent-encoded ASCII, and a host supplying raw UTF-8 gets it back. */
typedef struct optics_window_file_uri_to_path_opts {
    const char *uri;
    char *out_path;
    size_t out_cap;
} optics_window_file_uri_to_path_opts;

OPTICS_WINDOW_API int
optics_window_file_uri_to_path(const optics_window_file_uri_to_path_opts *opts);

#ifdef __cplusplus
}
#endif

#endif /* OPTICS_WINDOW_FILE_DIALOG_H */
